/* ============================================================
   一个调度 · bridge：ksu.exec 封装（多形态自适应）
   可靠性设计：
   1) 每次 exec 有超时，绝不让 Promise 悬挂；
   2) 返回值多形状归一化：JS 字段 / Java getter / result 嵌套 / 纯字符串；
   3) exec 调用形态启动探测一次并缓存：
      两参式（Promise 或 Java 对象）/ 三参回调式 / 直连脚本 execve；
   4) 候选目录探测不阻塞首屏；无 root 通道立刻给可读错误。

   真机教训：
   - v1.0.2：ksu.exec 返回 Java 对象（Kotlin data class），字段要走
     getStdout()/getErrno() getter 读，按 JS 字段读全是 undefined → 空输出；
   - v1.0.3：三参回调在该管理器直接抛 Java exception；
   - v1.0.1：KSU 安装后模块 .sh 可能是 0644，直连 execve 返回 EACCES。
   因此不再猜测形态——启动时用 version 命令探测，缓存可用形态。
   ============================================================ */
(function (global) {
  var FC = global.FC || (global.FC = {});

  // 经 /system/bin/sh 解释执行：模块脚本无执行位也能跑
  var SH = '/system/bin/sh';
  var SCRIPT = '/script/fcctl.sh';
  var CANDIDATE_DIRS = ['/data/adb/modules/WY925976742'];
  // 必须大于 fcctl.sh 后端看门狗的最长档（diag=10s）
  var DEFAULT_TIMEOUT = 12000;
  var PROBE_TIMEOUT = 8000;

  function now() { return Date.now(); }

  // 统一取值：先 JS 字段，再 Java getter 方法（getXxx()）
  function pick(o, jsName, javaName) {
    if (!o) return undefined;
    if (o[jsName] !== undefined) return o[jsName];
    if (javaName && typeof o[javaName] === 'function') {
      try { return o[javaName](); } catch (e) { /* ignore */ }
    }
    return undefined;
  }

  function normalize(res, cmd, started) {
    var out = { stdout: '', stderr: '', errno: 0, ms: now() - started, cmd: cmd };
    if (res === null || res === undefined) return out;
    if (res.__throw !== undefined) {
      out.thrown = true;
      out.stderr = String((res.__throw && res.__throw.message) || res.__throw);
      return out;
    }
    if (typeof res === 'string') { out.stdout = res; return out; }
    if (typeof res !== 'object') { out.stdout = String(res); return out; }
    out.__raw = res;  // 保存原始返回供 deepInspect 深度反射

    var e = pick(res, 'errno', 'getErrno');
    if (typeof e !== 'number') e = pick(res, 'code', 'getCode');
    if (typeof e === 'number') out.errno = e;
    var so = pick(res, 'stdout', 'getStdout');
    if (so !== undefined) out.stdout = String(so);
    var se = pick(res, 'stderr', 'getStderr');
    if (se !== undefined) out.stderr = String(se);

    var nested = pick(res, 'result', 'getResult');
    if (nested && typeof nested === 'object') {
      var e2 = pick(nested, 'errno', 'getErrno');
      if (typeof e2 === 'number') out.errno = e2;
      var s2 = pick(nested, 'stdout', 'getStdout');
      if (s2 !== undefined) out.stdout = String(s2);
      var r2 = pick(nested, 'stderr', 'getStderr');
      if (r2 !== undefined) out.stderr = String(r2);
    }
    return out;
  }

  function withTimeout(promise, ms, cmd, started) {
    return new Promise(function (resolve) {
      var done = false;
      var timer = setTimeout(function () {
        if (done) return;
        done = true;
        var r = normalize(null, cmd, started);
        r.timed_out = true;
        resolve(r);
      }, ms);
      Promise.resolve(promise).then(function (res) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        resolve(normalize(res, cmd, started));
      }, function (err) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        var r = normalize(null, cmd, started);
        r.stderr = String((err && err.message) || err);
        r.thrown = true;
        resolve(r);
      });
    });
  }

  var ksu = (function () {
    if (global.ksu && typeof global.ksu.exec === 'function') return global.ksu;
    if (global.android && typeof global.android.exec === 'function') return global.android;
    return null;
  })();

  // ---------------- 形态探测 ----------------
  // 'twoarg'    : exec(SH, argv) —— 返回 Promise 或 Java 对象（normalize 通吃）
  // 'callback'  : exec(SH, argv, cb) —— 结果走回调（部分管理器抛异常，捕获即跳过）
  // 'direct'    : exec(script, args) —— 直连脚本（依赖执行位，开机自愈后通常可用）
  // 'string'    : exec("sh script args") —— 单字符串命令行（某些版本 API 如此）
  // 'spawn-file': spawn(script, args) —— spawn 直连脚本（v1.0.7 真机发现 ksu 有 spawn 方法）
  // 'spawn-sh'  : spawn(SH, argv) —— spawn 经 sh 解释执行
  // 'raw-script': exec("script args") —— 纯脚本路径不加 sh（v1.0.8 真机试）
  // 'global-cb' : 设置全局回调后 exec，等回调给结果（v1.0.7 发现 exec 返回 undefined）
  // 'delayed-read': exec 后延迟扫描 window 新属性（结果可能存全局）
  // 'obj-param' : exec({command,args,timeout,onStdout,...}) 对象参数（v1.0.9 试）
  // 'sh-c'      : exec("sh -c '...'") 显式 sh -c 包装（某些 Java Runtime.exec 需要）
  // 'file-fetch': exec 重定向输出到 webroot/.fc_out，fetch() 经本地 HTTP 服务器读取
  //               （v1.0.10 核心：唯一能绕过 fire-and-forget 的通道）
  var modeCache = null;   // 探测结果；null=未探测
  var probing = null;
  var probeDiag = '';     // 探测全失败时的 API 真相摘要（用户截图一次即可匹配签名）

  function rawCall(mode, argv) {
    if (mode === 'direct') {
      return ksu.exec(argv[0], argv.slice(1));
    }
    if (mode === 'string') {
      // 单字符串命令行：整条命令拼成一个参数
      var line = SH + ' ' + argv[0];
      for (var i = 1; i < argv.length; i++) line += ' ' + argv[i];
      return ksu.exec(line);
    }
    if (mode === 'spawn-file') {
      return ksu.spawn(argv[0], argv.slice(1));
    }
    if (mode === 'spawn-sh') {
      return ksu.spawn(SH, argv);
    }
    if (mode === 'callback') {
      return new Promise(function (resolve, reject) {
        var settled = false;
        var maybe = ksu.exec(SH, argv, function (errno, stdout, stderr) {
          if (settled) return;
          settled = true;
          resolve({ errno: errno || 0, stdout: stdout || '', stderr: stderr || '' });
        });
        if (maybe && typeof maybe.then === 'function') {
          maybe.then(function (r) {
            if (!settled) { settled = true; resolve(r); }
          }, function (e) {
            if (!settled) { settled = true; reject(e); }
          });
        }
      });
    }
    if (mode === 'raw-script') {
      // 纯脚本路径：不加 sh 前缀，直接 exec 脚本+参数
      var rs = argv[0];
      for (var ri = 1; ri < argv.length; ri++) rs += ' ' + argv[ri];
      return ksu.exec(rs);
    }
    if (mode === 'global-cb') {
      // 全局回调模式：设置常见回调名后调用 exec，等回调给结果
      // （真机 v1.0.7：ksu.exec 返回 undefined，结果可能走全局回调）
      return new Promise(function (resolve, reject) {
        var settled = false;
        var cbFn = function (errno, stdout, stderr) {
          if (settled) return;
          settled = true;
          resolve({ errno: errno || 0, stdout: stdout || '', stderr: stderr || '' });
        };
        // 常见回调名全部指向同一函数
        var commonNames = ['__callback', 'ksuCallback', 'onExecResult', '__execResult',
                           'onSuccess', 'onResult', 'execCallback'];
        var saved = {};
        for (var ci = 0; ci < commonNames.length; ci++) {
          saved[commonNames[ci]] = global[commonNames[ci]];
          global[commonNames[ci]] = cbFn;
        }
        try {
          ksu.exec(SH, argv);
        } catch (e) {
          if (!settled) { settled = true; reject(e); return; }
        }
        // 3 秒内回调没来就放弃（清理全局回调）
        setTimeout(function () {
          if (settled) return;
          settled = true;
          for (var cj = 0; cj < commonNames.length; cj++) {
            if (saved[commonNames[cj]] === undefined) {
              try { delete global[commonNames[cj]]; } catch (e) { global[commonNames[cj]] = undefined; }
            } else {
              global[commonNames[cj]] = saved[commonNames[cj]];
            }
          }
          resolve({ errno: 0, stdout: '', stderr: '', __noCallback: true });
        }, 3000);
      });
    }
    if (mode === 'delayed-read') {
      // 延迟读全局变量：调用 exec 后等 1 秒，扫描 window 上新出现的属性
      // （结果可能被存到全局变量）。优先取含 "ok" 的值作为 stdout，
      // 否则拼接新属性名供诊断
      return new Promise(function (resolve, reject) {
        var beforeKeys = {};
        for (var bk in global) beforeKeys[bk] = true;
        try {
          ksu.exec(SH, argv);
        } catch (e) {
          reject(e);
          return;
        }
        setTimeout(function () {
          var newKeys = [];
          var found = '';
          for (var nk in global) {
            if (!beforeKeys[nk]) {
              try {
                var val = String(global[nk]);
                if (!found && val.indexOf('"ok"') >= 0) found = val;
                newKeys.push(nk + '=' + val.slice(0, 30));
              } catch (e) { /* ignore */ }
            }
          }
          resolve({ errno: 0, stdout: found || newKeys.join(' '), stderr: '' });
        }, 1000);
      });
    }
    if (mode === 'obj-param') return rawCallObj(argv);
    if (mode === 'sh-c') return rawCallShC(argv);
    if (mode === 'file-fetch') return rawCallFileFetch(argv);
    return ksu.exec(SH, argv);
  }

  // file-fetch 形态：ksu.exec 重定向输出到 webroot/.fc_out，
  // fetch() 经 WebUI 本地 HTTP 服务器读取（v1.0.10 核心）。
  // 原理：ksu.exec 是 fire-and-forget（返回 undefined），但命令确实执行；
  // 输出重定向到 webroot/ 后，fetch() 可经本地 HTTP 服务器读到。
  //
  // v1.0.15 竞态根治（唯一结束标记 + 串行队列）：
  // 旧实现「轮询到任何非空内容就接受」有两个致命窗口——① shell 尚未
  // truncate，读到上个命令的残留（陈旧内容）；② HTTP 读取撞上写入中，
  // 拿到半写残片。真机现象：应用页「设备端返回不是合法 JSON」+列表空白。
  // 新协议：命令完成后追加唯一结束标记 __FCDONE_<reqId>__，轮询只有
  // 见到本请求的标记才认账（标记前内容即本命令输出）。陈旧内容绝不含
  // 本请求标记，半写内容无标记——两类竞态同时根除。
  // 串行队列：同一时刻只允许一个 file-fetch 命令操作 .fc_out，
  // 杜绝并发请求（总览轮询 vs 页面加载）互相截断。
  var ffQueue = Promise.resolve();

  function rawCallFileFetch(argv) {
    // fetch 不可用（非 HTTP 环境/mock 测试）时立即跳过，不浪费 250ms
    if (typeof fetch !== 'function') {
      return Promise.resolve({ errno: 0, stdout: '', stderr: 'fetch 不可用', __noOutput: true });
    }
    var reqId = '__FCDONE_' + Date.now().toString(36) +
                Math.random().toString(36).slice(2, 8) + '__';
    var outPath = CANDIDATE_DIRS[0] + '/webroot/.fc_out';
    var cmd = SH + ' ' + argv[0];
    for (var i = 1; i < argv.length; i++) cmd += ' ' + argv[i];
    // 单字符串经 sh 解释（现有 > 重定向已证明该通道成立，; 同样成立）：
    // 先截断写命令输出，命令完成后追加唯一结束标记
    var line = cmd + ' > ' + outPath + ' 2>&1; echo ' + reqId + ' >> ' + outPath;

    var run = function () {
      // 执行（fire-and-forget）
      try { ksu.exec(line); } catch (e) { /* ignore */ }
      // 轮询 fetch 读取输出（250ms × 40 次 ≈ 10s，与后端看门狗最长档匹配）。
      // 只有见到本请求的结束标记才认账——标记之前的内容即本命令输出。
      return new Promise(function (resolve) {
        var attempts = 0;
        var maxAttempts = 40;
        function poll() {
          attempts++;
          try {
            fetch('.fc_out?t=' + Date.now()).then(function (resp) {
              return resp.text();
            }).then(function (text) {
              var idx = text ? text.indexOf(reqId) : -1;
              if (idx >= 0) {
                // 标记之前即输出；剥掉标记行与尾部换行
                var out = text.slice(0, idx).replace(/\n+$/, '');
                resolve({ errno: 0, stdout: out, stderr: '' });
              } else if (attempts >= maxAttempts) {
                resolve({ errno: 0, stdout: '', stderr: 'file-fetch 等待超时', __noOutput: true });
              } else {
                setTimeout(poll, 250);
              }
            }).catch(function () {
              if (attempts >= maxAttempts) {
                resolve({ errno: 0, stdout: '', stderr: 'file-fetch 读取失败', __noOutput: true });
              } else {
                setTimeout(poll, 250);
              }
            });
          } catch (e) {
            resolve({ errno: 0, stdout: '', stderr: String(e), __noOutput: true });
          }
        }
        setTimeout(poll, 250);
      });
    };

    // 入队：前一个 file-fetch 请求落定后才执行本请求；
    // 单请求失败不中断队列（ffQueue 本身永不 reject）
    var result = ffQueue.then(run, run);
    ffQueue = result.then(function () {}, function () {});
    return result;
  }

  // obj-param 形态：对象参数 + 回调字段（某些 WebUI 框架 API）
  function rawCallObj(argv) {
    var line = argv[0];
    for (var i = 1; i < argv.length; i++) line += ' ' + argv[i];
    return new Promise(function (resolve, reject) {
      var settled = false;
      var finish = function (errno, stdout, stderr) {
        if (settled) return;
        settled = true;
        resolve({ errno: errno || 0, stdout: stdout || '', stderr: stderr || '' });
      };
      try {
        var ret = ksu.exec({
          command: line,
          args: argv.slice(1),
          timeout: 8000,
          onStdout: function (s) { finish(0, s, ''); },
          onStderr: function (s) { finish(0, '', s); },
          onExit: function (code, stdout, stderr) { finish(code, stdout, stderr); },
          callback: function (errno, stdout, stderr) { finish(errno, stdout, stderr); },
          success: function (stdout) { finish(0, stdout, ''); },
          error: function (stderr) { finish(1, '', stderr); }
        });
        // 返回值若是 thenable 或带输出字段，也一并采集
        if (ret && typeof ret.then === 'function') {
          ret.then(function (r) {
            if (settled) return;
            settled = true;
            resolve(r);
          }, function (e) {
            if (settled) return;
            settled = true;
            reject(e);
          });
        } else if (ret !== undefined && ret !== null) {
          // 返回了非空对象/字符串——归一化取值
          setTimeout(function () {
            if (settled) return;
            settled = true;
            var so = '';
            try {
              if (typeof ret === 'string') so = ret;
              else if (ret.stdout !== undefined) so = String(ret.stdout);
              else if (typeof ret.getStdout === 'function') so = String(ret.getStdout());
              else if (typeof ret.getOut === 'function') so = String(ret.getOut());
              else so = String(ret);
            } catch (e) { /* ignore */ }
            resolve({ errno: 0, stdout: so, stderr: '' });
          }, 100);
        }
        // 3 秒兜底
        setTimeout(function () {
          if (settled) return;
          settled = true;
          resolve({ errno: 0, stdout: '', stderr: '', __noCallback: true });
        }, 3000);
      } catch (e) {
        if (!settled) { settled = true; reject(e); }
      }
    });
  }

  // sh-c 形态：显式 sh -c 包装（某些 Java Runtime.exec 需要 shell 解析）
  function rawCallShC(argv) {
    var inner = argv[0];
    for (var i = 1; i < argv.length; i++) inner += ' ' + argv[i];
    return ksu.exec(SH + " -c '" + inner + "'");
  }

  function looksOk(r) {
    if (r.timed_out || r.thrown) return false;
    if (r.stdout && r.stdout.indexOf('"ok"') >= 0) return true;
    // normalize 读不到 stdout 时（getter 名未知场景），扫描原始返回的
    // 字段与函数调用结果——只要有一个字符串含 "ok" 即视为该形态可用
    if (r.__raw) {
      try {
        for (var k in r.__raw) {
          var v = r.__raw[k];
          if (typeof v === 'string' && v.indexOf('"ok"') >= 0) return true;
          if (typeof v === 'function') {
            var rv = v.call(r.__raw);
            if (typeof rv === 'string' && rv.indexOf('"ok"') >= 0) return true;
          }
        }
      } catch (e) { /* ignore */ }
    }
    return false;
  }

  // 形态返回摘要：errno + stdout 实际内容（截断）+ thrown/timeout 一行一个，
  // 全失败时用户截图即可匹配真实签名（v1.0.5 真机四形态全败的定位手段）
  function summarize(r) {
    if (!r) return 'null';
    var parts = [];
    parts.push('errno=' + (r.errno === undefined ? '-' : r.errno));
    var so = r.stdout ? String(r.stdout) : '';
    parts.push('so=' + so.length + ':' + JSON.stringify(so.slice(0, 30)));
    parts.push('th=' + (r.thrown ? 1 : 0));
    parts.push('to=' + (r.timed_out ? 1 : 0));
    return parts.join(',');
  }

  // 深度反射：dump 返回对象的 typeof、ownKeys、proto 方法名，
  // 并逐一采样调用候选 getter 带截断值——下一张截图即可精确匹配字段名。
  // 真机教训（v1.0.6 七测）：返回对象的字段名不是 stdout/getStdout/errno 等
  // 常见名，normalize 全部读空；必须反射出真实 getter 才能适配。
  var CANDIDATE_GETTERS = [
    'getOut', 'getOutput', 'getStdOut', 'getStdErr', 'getError', 'getErr',
    'getExitCode', 'getResult', 'getStdout', 'getStderr', 'getErrno',
    'data', 'out', 'err', 'code', 'output', 'stdout', 'stderr', 'errno', 'result'
  ];

  function deepInspect(res) {
    if (res === null) return 'null';
    if (res === undefined) return 'undefined';
    var t = typeof res;
    if (t === 'string') return 'str(' + res.length + ')' + JSON.stringify(res.slice(0, 30));
    if (t !== 'object') return t + ':' + String(res).slice(0, 30);
    var keys = [];
    try { keys = Object.keys(res); } catch (e) { /* ignore */ }
    var proto = [];
    try {
      var p = Object.getPrototypeOf(res);
      if (p && p !== Object.prototype) {
        proto = Object.getOwnPropertyNames(p).filter(function (k) { return k !== 'constructor'; });
      }
    } catch (e) { /* ignore */ }
    // 采样候选 getter/字段
    var samples = [];
    for (var i = 0; i < CANDIDATE_GETTERS.length; i++) {
      var k = CANDIDATE_GETTERS[i];
      try {
        var v = res[k];
        if (typeof v === 'function') {
          var rv = v.call(res);
          samples.push(k + '()=' + JSON.stringify(String(rv === undefined ? 'undef' : rv).slice(0, 20)));
        } else if (v !== undefined) {
          samples.push(k + '=' + JSON.stringify(String(v).slice(0, 20)));
        }
      } catch (e) { /* ignore */ }
    }
    return 'obj keys=[' + keys.join(',') + '] proto=[' + proto.join(',') + '] ' +
           samples.join(' ');
  }

  function buildProbeDiag() {
    var keys = [];
    try {
      for (var k in ksu) keys.push(k);
    } catch (e) { /* ignore */ }
    // fn.toString()：如果 ksu.exec 是 JS 函数（非纯 Java 桥接），
    // toString() 会显示源码——直接看到它内部怎么调用、结果从哪来（v1.0.8 核心）
    var execSrc = '';
    try {
      if (typeof ksu.exec === 'function') {
        execSrc = ksu.exec.toString().slice(0, 200);
      }
    } catch (e) { execSrc = 'toString-fail:' + ((e && e.message) || e); }
    // moduleInfo：完整 JSON（截断 500）——mockDir 路径可能揭示输出文件位置
    var miInfo = '';
    try {
      if (typeof ksu.moduleInfo === 'function') {
        var mi = ksu.moduleInfo();
        miInfo = 'mi=' + JSON.stringify(String(mi).slice(0, 500));
      }
    } catch (e) { miInfo = 'mi-fail:' + ((e && e.message) || e); }
    // getPackageInfo / listPackages：可能含 API 版本/接口信息
    var pkgInfo = '';
    try {
      if (typeof ksu.getPackageInfo === 'function') {
        var pi = ksu.getPackageInfo('com.android.shell');
        pkgInfo = 'pi=' + JSON.stringify(String(pi).slice(0, 120));
      }
    } catch (e) { pkgInfo = 'pi-fail:' + ((e && e.message) || e); }
    var lpInfo = '';
    try {
      if (typeof ksu.listPackages === 'function') {
        var lp = ksu.listPackages();
        lpInfo = 'lp=' + JSON.stringify(String(lp).slice(0, 80));
      }
    } catch (e) { lpInfo = 'lp-fail:' + ((e && e.message) || e); }
    // 全局命名空间 dump：找其他 exec 入口（window.webui / window.module 等）
    // 全量列出（不截断），找末尾的自定义桥接对象
    var nsKeys = [];
    try {
      for (var gk in global) {
        if (gk === 'ksu' || gk === 'android' || gk === 'FC') continue;
        if (/^(window|document|location|navigator|screen|history|console|performance|fetch|XMLHttpRequest|setTimeout|setInterval|clearTimeout|clearInterval|requestAnimationFrame|cancelAnimationFrame|Promise|Object|Array|String|Number|Boolean|Date|Math|JSON|RegExp|Error|TypeError|ReferenceError|SyntaxError|RangeError|EvalError|URIError|Function|Symbol|Map|Set|WeakMap|WeakSet|Proxy|Reflect|BigInt|Infinity|NaN|undefined|eval|isNaN|isFinite|parseInt|parseFloat|encodeURI|decodeURI|encodeURIComponent|decodeURIComponent|escape|unescape|alert|confirm|prompt|open|close|focus|blur|print|postMessage|addEventListener|removeEventListener|dispatchEvent|CustomEvent|Event|EventTarget|AbortController|AbortSignal|Headers|Request|Response|ReadableStream|WritableStream|TransformStream|TextEncoder|TextDecoder|URL|URLSearchParams|FormData|Blob|File|FileReader|MediaSource|Audio|Image|Video|localStorage|sessionStorage|indexedDB|crypto|matchMedia|getComputedStyle|requestIdleCallback|cancelIdleCallback|queueMicrotask|structuredClone|reportError|trustedTypes|webkitRequestAnimationFrame|webkitCancelAnimationFrame)$/.test(gk)) continue;
        try {
          var gv = global[gk];
          var gt = typeof gv;
          if (gt === 'function') nsKeys.push(gk + '(fn' + (gv.length !== undefined ? ':' + gv.length : '') + ')');
          else if (gt === 'object' && gv !== null) {
            var gk2 = [];
            try { for (var k2 in gv) gk2.push(k2); } catch (e2) { /* ignore */ }
            nsKeys.push(gk + '{' + gk2.slice(0, 8).join(',') + '}');
          } else nsKeys.push(gk + '=' + gt);
        } catch (e) { /* ignore */ }
      }
    } catch (e) { /* ignore */ }
    // window.chrome dump：Android WebView 的 chrome 对象可能藏自定义方法
    var chromeInfo = '';
    try {
      if (global.chrome) {
        var ck = [];
        for (var c in global.chrome) ck.push(c);
        chromeInfo = ' chrome=[' + ck.join(',') + ']';
      }
    } catch (e) { chromeInfo = ' chrome-fail'; }
    // webkit.messageHandlers 探测（iOS 风格消息处理器）
    var wkInfo = '';
    try {
      if (global.webkit && global.webkit.messageHandlers) {
        var wk = [];
        for (var w in global.webkit.messageHandlers) wk.push(w);
        wkInfo = ' webkit=[' + wk.join(',') + ']';
      }
    } catch (e) { wkInfo = ' webkit-fail'; }
    return 'ksu.exec(' +
        (typeof ksu.exec === 'function' ? 'len:' + (ksu.exec.length === undefined ? '?' : ksu.exec.length) : typeof ksu.exec) +
        ') keys=[' + keys.join(',') + '] src=' + JSON.stringify(execSrc) + ' ' + miInfo + ' ' + pkgInfo + ' ' + lpInfo +
        ' ns=[' + nsKeys.join(' ') + ']' + chromeInfo + wkInfo + ' ' +
        probeDiag;
  }

  function probeMode() {
    if (modeCache) return Promise.resolve(modeCache);
    if (probing) return probing;
    var probeArgv = [CANDIDATE_DIRS[0] + SCRIPT, 'version'];
    var forms = ['file-fetch', 'twoarg', 'callback', 'direct', 'string', 'spawn-file', 'spawn-sh',
                 'raw-script', 'global-cb', 'delayed-read', 'obj-param', 'sh-c'];
    var i = 0;
    var diags = [];
    // console 拦截：ksu.exec 可能把输出写到 console——拦截后调用 exec，看是否有输出
    var consoleBuf = [];
    var origLog = console.log;
    var origErr = console.error;
    console.log = function () {
      try { consoleBuf.push('[log] ' + Array.prototype.join.call(arguments, ' ')); } catch (e) { /* ignore */ }
      try { origLog.apply(console, arguments); } catch (e) { /* ignore */ }
    };
    console.error = function () {
      try { consoleBuf.push('[err] ' + Array.prototype.join.call(arguments, ' ')); } catch (e) { /* ignore */ }
      try { origErr.apply(console, arguments); } catch (e) { /* ignore */ }
    };
    probing = (function next() {
      if (i >= forms.length) return Promise.resolve(null);
      var m = forms[i++];
      var started = now();
      var p;
      try {
        p = Promise.resolve(rawCall(m, probeArgv));
      } catch (e) {
        diags.push(m + ':throw:' + ((e && e.message) || e));
        return next();  // 该形态直接抛异常（如三参回调），换下一种
      }
      return withTimeout(p, PROBE_TIMEOUT, 'probe', started).then(function (r) {
        // 每个形态都记录深度反射（含匹配的）——若匹配形态 normalize 读不到
        // stdout，反射帮我们找到真实 getter
        diags.push(m + ':' + summarize(r) + ' ' + deepInspect(r.__raw));
        if (looksOk(r)) return m;
        return next();
      });
    })().then(function (m) {
      modeCache = m || 'twoarg';  // 探测失败默认两参式（错误会如实呈现）
      probing = null;
      // 恢复 console，把拦截到的输出拼进诊断
      console.log = origLog;
      console.error = origErr;
      var consoleInfo = '';
      if (consoleBuf.length > 0) {
        consoleInfo = ' console=[' + consoleBuf.slice(0, 5).join(' | ') + ']';
      }
      probeDiag = diags.join(' | ') + consoleInfo;
      return modeCache;
    });
    return probing;
  }

  FC.bridge = {
    timeout: DEFAULT_TIMEOUT,
    hasRoot: function () { return !!ksu; },
    mode: function () { return modeCache; },
    // 探测全失败时的 API 真相摘要（含 exec 签名/对象键/各形态返回）
    diag: function () { return buildProbeDiag(); },

    // exec(args...) -> Promise<{stdout,stderr,errno,ms,cmd,timed_out,thrown}>
    exec: function (args, opts) {
      var self = this;
      opts = opts || {};
      var timeout = opts.timeout || DEFAULT_TIMEOUT;
      var cmd = 'fcctl.sh ' + Array.prototype.join.call(args, ' ');
      var started = now();

      if (!ksu) {
        return Promise.resolve((function () {
          var r = normalize(null, cmd, started);
          r.stderr = '未检测到 root 通道（window.ksu.exec 不存在）';
          r.thrown = true;
          return r;
        })());
      }

      var dirs = opts.dirs || CANDIDATE_DIRS;
      // 先确保形态已探测（仅首次），再按候选目录执行
      return probeMode().then(function () {
        var attempt = function (i) {
          if (i >= dirs.length) {
            var r = normalize(null, cmd, started);
            r.stderr = '模块目录探测失败（已尝试 ' + dirs.length + ' 个候选路径）';
            r.thrown = true;
            return r;
          }
          var argv = [dirs[i] + SCRIPT].concat(Array.prototype.slice.call(args));
          var p;
          try {
            p = Promise.resolve(rawCall(modeCache, argv));
          } catch (e) {
            p = Promise.reject(e);
          }
          return withTimeout(p, timeout, cmd, started).then(function (r) {
            // 脚本不存在（errno 2 或提示 missing）时尝试下一个候选目录
            var missing = (r.errno === 2) ||
                          (r.stderr && /no such file|not found/i.test(r.stderr));
            if (missing && i + 1 < dirs.length) return attempt(i + 1);
            return r;
          });
        };
        return attempt(0);
      });
    }
  };
})(window);
