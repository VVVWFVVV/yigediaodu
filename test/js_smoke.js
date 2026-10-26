/* ============================================================
   一个调度 · 前端冒烟测试（Node 环境，无浏览器）
   验证：1) 全部 JS 可加载；2) boot 与四个页面的 render/load 不抛异常；
        3) ksu.exec 通道异常时有可读错误而非静默。
   运行：node test/js_smoke.js
   ============================================================ */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { TextDecoder } = require('util');

const DIR = path.join(__dirname, '..', 'webroot', 'assets');
const FILES = ['bridge.js', 'api.js', 'app.js', 'pages.js', 'page-settings.js', 'page-diag.js'];

let pass = 0;
let fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log('  PASS  ' + name); }
  else { fail++; console.log('  FAIL  ' + name + (extra ? '  [' + extra + ']' : '')); }
}

// ---------------- 最小 DOM mock ----------------
const registry = new Map();

function makeEl(tag) {
  const e = {
    tagName: tag || 'div',
    children: [],
    style: {},
    className: '',
    attrs: {},
    textContent: '',
    value: '',
    checked: true,
    type: '',
    disabled: false,
    _html: '',
    _id: '',
    classList: {
      _s: new Set(),
      add(c) { this._s.add(c); },
      remove(c) { this._s.delete(c); },
      toggle(c, f) { if (f === undefined) f = !this._s.has(c); if (f) this._s.add(c); else this._s.delete(c); return f; },
      contains(c) { return this._s.has(c); }
    },
    setAttribute(k, v) { this.attrs[k] = String(v); },
    getAttribute(k) { return Object.prototype.hasOwnProperty.call(this.attrs, k) ? this.attrs[k] : null; },
    appendChild(c) {
      if (c && c._isFragment) {
        c.children.slice().forEach((g) => this.appendChild(g));
        return c;
      }
      this.children.push(c);
      if (c && c._id) registry.set(c._id, c);
      return c;
    },
    get lastChild() { return this.children.length ? this.children[this.children.length - 1] : null; },
    get firstChild() { return this.children.length ? this.children[0] : null; },
    addEventListener() {},
    removeEventListener() {},
    querySelector() { return makeEl('div'); },
    querySelectorAll() { return []; },
    focus() {},
    select() {},
    click() { if (this._onclick) this._onclick(); }
  };
  Object.defineProperty(e, 'id', {
    get() { return this._id; },
    set(v) { this._id = v; registry.set(v, this); }
  });
  Object.defineProperty(e, 'innerHTML', {
    get() { return this._html; },
    set(v) { this._html = v; this.children = []; }
  });
  return e;
}

const viewEl = makeEl('main');
viewEl.id = 'fc-view';
const errbarEl = makeEl('div');
errbarEl.id = 'fc-errbar';
const errbarText = makeEl('span');
errbarText.id = 'fc-errbar-text';
const errbarClose = makeEl('button');
errbarClose.id = 'fc-errbar-close';
const versionEl = makeEl('span');
versionEl.id = 'fc-version';
const daemonBadge = makeEl('span');
daemonBadge.id = 'fc-daemon-badge';
const enabledSw = makeEl('input');
enabledSw.id = 'fc-enabled';
enabledSw.checked = true;
const tabsEl = makeEl('nav');
tabsEl.id = 'fc-tabs';
['overview', 'apps', 'settings', 'diag'].forEach(function (r) {
  const t = makeEl('button');
  t.setAttribute('data-route', r);
  tabsEl.children.push(t);
});

function makeFragment() {
  const f = makeEl('#fragment');
  f._isFragment = true;
  return f;
}

const documentMock = {
  createElement: makeEl,
  createDocumentFragment: makeFragment,
  getElementById: function (id) {
    const map = {
      'fc-view': viewEl, 'fc-errbar': errbarEl, 'fc-errbar-text': errbarText,
      'fc-errbar-close': errbarClose, 'fc-version': versionEl,
      'fc-daemon-badge': daemonBadge, 'fc-enabled': enabledSw, 'fc-tabs': tabsEl
    };
    if (Object.prototype.hasOwnProperty.call(map, id)) return map[id];
    return registry.get(id) || null;
  },
  querySelectorAll: function () { return []; },
  querySelector: function () { return makeEl('div'); },
  addEventListener() {},
  body: makeEl('body'),
  documentElement: makeEl('html')
};

// ---------------- ksu.exec mock ----------------
const STATUS = JSON.stringify({
  ok: true, data: {
    version: '1.0.0', mode: 'host',
    pkg: 'com.tencent.tmgp.sgame', profile: 'game',
    caps: { cpu: 'ok', cpuset: 'ok', threads: 'ok', io: 'ok', mem: 'ok', thermal: 'off', fps: 'ok' }
  }
});
const LIST = JSON.stringify({ ok: true, data: [{ pkg: 'com.miHoYo.Yuanshen', mode: 'game' }] });
const SETTINGS_B64 = Buffer.from('enabled=1\npoll_interval_ms=1000\nfps.enable=1\n').toString('base64');
const SETTINGS = JSON.stringify({ ok: true, data: SETTINGS_B64 });
const LIVE = JSON.stringify({ ok: true, data: { PKG: 'com.tencent.tmgp.sgame', FAMILY: 'game', TEMP: '45', FREQ: '1152000,1958400', FPS_LEVEL: '1', THERM: '0', FPS: '60' } });
const LOGS_B64 = Buffer.from('10-18 17:00:00 [INFO] 测试日志\n').toString('base64');
const LOGS = JSON.stringify({ ok: true, data: LOGS_B64 });
const PROFILES = JSON.stringify({ ok: true, data: ['daily', 'daily-plus', 'game', 'bg', 'off'] });
const SELFCHECK = JSON.stringify({ ok: true, data: '[selfcheck] PASS' });
const DIAG_B64 = Buffer.from('一个调度 体检\n[scx] 只读探测\n').toString('base64');
const DIAG = JSON.stringify({ ok: true, data: DIAG_B64 });
const PROBE_FG = JSON.stringify({ ok: true, data: { pkg: 'com.tencent.tmgp.sgame', oom_adj: 0, from_dumpsys: false } });
const PROBE_SCREEN = '{"ok":true,"data":"on"}\n';
const DAEMON_STATUS = JSON.stringify({ ok: true, data: { running: true, pid: 1234 } });

// exec 形态：ksu.exec("/system/bin/sh", [脚本路径, 命令, 参数...])
// mock 校验第一参数为脚本路径（证明经 sh 调用，不依赖执行位），
// 命令取 args[1]
function mockExec(script, args) {
    if (script !== '/system/bin/sh' || !args || !args[0] || args[0].indexOf('fcctl.sh') < 0) {
        return Promise.resolve({ errno: 2, stdout: '', stderr: 'no such file' });
    }
    const cmd = args[1] || '';
    let out = '{"ok":true,"data":{}}';
  switch (cmd) {
      case 'version': out = '{"ok":true,"data":"1.0.0"}'; break;
    case 'status': out = STATUS; break;
    case 'list': out = LIST; break;
    case 'settings': out = SETTINGS; break;
    case 'live': out = LIVE; break;
    case 'logs': out = LOGS; break;
    case 'profiles': out = PROFILES; break;
    case 'getprofile': out = SETTINGS; break;
    case 'selfcheck': out = SELFCHECK; break;
    case 'diag': out = DIAG; break;
    case 'probe': out = args[1] === 'fg' ? PROBE_FG : PROBE_SCREEN; break;
    case 'daemon': out = DAEMON_STATUS; break;
    case 'export': out = LOGS; break;
    case 'enable': case 'setsetting': case 'apply': case 'pause': case 'resume': case 'reload': case 'mode': case 'delapp':
      out = '{"ok":true,"data":true}'; break;
    default: out = '{"ok":false,"msg":"未知命令"}';
  }
  return Promise.resolve({ errno: 0, stdout: out, stderr: '' });
}

// ---------------- 沙箱 ----------------
const sandbox = {
  console, setTimeout, clearTimeout, setInterval, clearInterval,
  addEventListener() {}, removeEventListener() {},
  Promise, Date, JSON, Math, Object, Array, String, Number, Boolean, RegExp, Error,
  TextDecoder, Uint8Array, atob: (s) => Buffer.from(s, 'base64').toString('binary'),
  btoa: (s) => Buffer.from(s, 'binary').toString('base64'),
  navigator: { clipboard: null },
  location: { hash: '' },
  document: documentMock
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;
sandbox.ksu = { exec: mockExec };
vm.createContext(sandbox);

console.log('[js-smoke] 加载面板脚本…');
for (const f of FILES) {
  const code = fs.readFileSync(path.join(DIR, f), 'utf8');
  try {
    vm.runInContext(code, sandbox, { filename: f });
    ok('加载 ' + f, true);
  } catch (e) {
    ok('加载 ' + f, false, e.message);
  }
}

ok('FC 命名空间就绪', !!sandbox.FC);
ok('bridge 就绪', !!(sandbox.FC && sandbox.FC.bridge));
// bridge 经 sh 调用验证：移入 form-probe 串行链（file-fetch 首位探测
// 引入异步边界，独立 IIFE 的 mock 会与 boot/renderRoute 调用交错污染）

// 双模与形态探测用例。
// 关键：exec 经形态探测是异步的，页面用例的异步链（list/status 等）与
// 本链会交叉——必须放在页面断言完成之后串行跑，且前一个断言完成
// （mock 已恢复）再跑下一个，否则微任务里 mock 已被换掉。
var runFormProbeTests = (function () {
    // 重新加载 bridge.js 获得全新的形态探测状态（modeCache 在闭包内）
    function reloadBridge() {
        const fsLocal = require('fs');
        const pathLocal = require('path');
        const src = fsLocal.readFileSync(pathLocal.join(__dirname, '..', 'webroot', 'assets', 'bridge.js'), 'utf8');
        vm.runInContext(src, sandbox, { filename: 'bridge.js' });
    }

    return function () {
    var oldExec0 = sandbox.ksu.exec;
    var chain = Promise.resolve();

    // sh 调用验证（串行链内，reloadBridge 后 mock 隔离）
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prev = sandbox.ksu.exec;
            var captured = null;
            sandbox.ksu.exec = function (cmd, args) {
                if (typeof cmd === 'string' && cmd === '/system/bin/sh' && args && args.length >= 2) {
                    captured = { cmd: cmd, args: args };
                    return Promise.resolve({ errno: 0, stdout: '{"ok":true,"data":{}}', stderr: '' });
                }
                return undefined;
            };
            sandbox.FC.bridge.exec(['version']).then(function () {
                sandbox.ksu.exec = prev;
                ok('bridge 经 sh 调用（无执行位依赖）',
                   captured && captured.cmd === '/system/bin/sh' &&
                   captured.args && captured.args[0].indexOf('fcctl.sh') >= 0 &&
                   captured.args[1] === 'version');
                resolve();
            }, function () {
                sandbox.ksu.exec = prev;
                ok('bridge 经 sh 调用（无执行位依赖）', false, 'exec 失败');
                resolve();
            });
        });
    });

    function step(name, installMock) {
        return function () {
            return new Promise(function (resolve) {
                reloadBridge();
                var prev = sandbox.ksu.exec;
                sandbox.ksu.exec = installMock();
                var settled = false;
                function done(v, errMsg) {
                    if (settled) return;
                    settled = true;
                    sandbox.ksu.exec = prev;
                    ok(name, errMsg ? false : v === '1.0.0', errMsg || ('got: ' + v));
                    resolve();
                }
                sandbox.FC.api.call('version').then(function (v) { done(v, null); },
                                                    function (e) { done(null, e.message); });
            });
        };
    }

    // 回调式（v1.0.3 起）：两参返回 undefined，三参回调给结果
    chain = chain.then(step('回调式 ksu.exec 双模兼容', function () {
        return function (cmd, args, cb) {
            if (typeof cb !== 'function') return undefined;
            setTimeout(function () { cb(0, '{"ok":true,"data":"1.0.0"}\n', ''); }, 5);
            return undefined;
        };
    }));

    // 形态一：Java 对象式（Kotlin data class——字段走 getStdout()/getErrno() getter）
    // 真机教训：v1.0.2 按 JS 字段读全 undefined，前端只看到「无输出」
    chain = chain.then(step('Java 对象式 exec 探测兼容', function () {
        return function (cmd, args) {
            return {
                getErrno: function () { return 0; },
                getStdout: function () { return '{"ok":true,"data":"1.0.0"}\n'; },
                getStderr: function () { return ''; }
            };
        };
    }));

    // 形态二：回调式（两参 undefined，三参回调）
    chain = chain.then(step('回调式 exec 探测兼容', function () {
        return function (cmd, args, cb) {
            if (typeof cb !== 'function') return undefined;
            setTimeout(function () { cb(0, '{"ok":true,"data":"1.0.0"}\n', ''); }, 5);
            return undefined;
        };
    }));

    // 形态三：直连脚本式（sh 调用不被支持，只有直连 execve 脚本可行）
    chain = chain.then(step('直连脚本式 exec 探测兼容', function () {
        return function (cmd, args, cb) {
            var isScript = cmd.indexOf('fcctl.sh') >= 0;
            var out = isScript ? '{"ok":true,"data":"1.0.0"}\n' : 'sh: not supported\n';
            if (typeof cb === 'function') { cb(0, out, ''); return undefined; }
            return Promise.resolve({ errno: 0, stdout: out, stderr: '' });
        };
    }));

    // 形态四：单字符串命令行式（只接受 exec("sh script args") 整串）
    chain = chain.then(step('单字符串命令行 exec 探测兼容', function () {
        return function (cmd, args, cb) {
            var isLine = typeof cmd === 'string' && cmd.indexOf('fcctl.sh') >= 0 && args === undefined;
            var out = isLine ? '{"ok":true,"data":"1.0.0"}\n' : 'unsupported\n';
            if (typeof cb === 'function') { cb(0, out, ''); return undefined; }
            return Promise.resolve({ errno: 0, stdout: out, stderr: '' });
        };
    }));

    // 全形态失败时必须 dump API 真相（exec 签名/对象键/各形态返回摘要），
    // 用户截图一次即可匹配真实签名（v1.0.5 真机四形态全败的定位手段）
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prev = sandbox.ksu.exec;
            sandbox.ksu.exec = function () { return Promise.resolve({ errno: 2, stdout: '', stderr: 'unknown api' }); };
            var settled = false;
            sandbox.FC.api.call('version').then(function () {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prev;
                ok('全形态失败时错误含 API 真相摘要', false, '竟然成功了？');
                resolve();
            }, function (e) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prev;
                var msg = e.message || '';
                ok('全形态失败时错误含 API 真相摘要',
                   msg.indexOf('ksu.exec(') >= 0 && msg.indexOf('keys=[') >= 0 && msg.indexOf('twoarg:') >= 0,
                   msg.slice(0, 120));
                resolve();
            });
        });
    });

    // 深度反射断言：错误消息必须含 proto（原型方法名）与采样 getter 值
    // （v1.0.7 真机定位：返回对象字段名不是 stdout/getStdout，需反射）
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prev = sandbox.ksu.exec;
            // 返回一个「非标准 getter 名」的 Java 对象——normalize 读不到 stdout
            sandbox.ksu.exec = function () {
                return {
                    getOut: function () { return '{"ok":true,"data":"1.0.0"}\n'; },
                    getCode: function () { return 0; }
                };
            };
            var settled = false;
            sandbox.FC.api.call('version').then(function () {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prev;
                // getOut 含 "ok"，looksOk raw 扫描应识别为可用形态
                ok('深度反射含 proto 与采样 getter', false, '竟然成功了？');
                resolve();
            }, function (e) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prev;
                var msg = e.message || '';
                ok('深度反射含 proto 与采样 getter',
                   msg.indexOf('proto=') >= 0 && msg.indexOf('getOut()') >= 0,
                   msg.slice(0, 150));
                resolve();
            });
        });
    });

    // spawn 形态：ksu.spawn(script, args) 直连脚本
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prevExec = sandbox.ksu.exec;
            var prevSpawn = sandbox.ksu.spawn;
            sandbox.ksu.exec = function () { return undefined; };  // exec 不可用
            sandbox.ksu.spawn = function (file, args) {
                var isScript = String(file).indexOf('fcctl.sh') >= 0;
                return Promise.resolve({ errno: 0, stdout: isScript ? '{"ok":true,"data":"1.0.0"}\n' : '', stderr: '' });
            };
            var settled = false;
            sandbox.FC.api.call('version').then(function (v) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                sandbox.ksu.spawn = prevSpawn;
                ok('spawn-file 形态探测兼容', v === '1.0.0', 'got: ' + v);
                resolve();
            }, function (e) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                sandbox.ksu.spawn = prevSpawn;
                ok('spawn-file 形态探测兼容', false, e.message);
                resolve();
            });
        });
    });

    // spawn-sh 形态：ksu.spawn(SH, argv) 经 sh 解释执行
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prevExec = sandbox.ksu.exec;
            var prevSpawn = sandbox.ksu.spawn;
            sandbox.ksu.exec = function () { return undefined; };  // exec 不可用
            sandbox.ksu.spawn = function (file, args) {
                var isSh = String(file).indexOf('/sh') >= 0;
                return Promise.resolve({ errno: 0, stdout: isSh ? '{"ok":true,"data":"1.0.0"}\n' : '', stderr: '' });
            };
            var settled = false;
            sandbox.FC.api.call('version').then(function (v) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                sandbox.ksu.spawn = prevSpawn;
                ok('spawn-sh 形态探测兼容', v === '1.0.0', 'got: ' + v);
                resolve();
            }, function (e) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                sandbox.ksu.spawn = prevSpawn;
                ok('spawn-sh 形态探测兼容', false, e.message);
                resolve();
            });
        });
    });

    // global-cb 形态：ksu.exec 返回 undefined，结果走全局回调
    // （v1.0.7 真机核心发现：exec 是 fire-and-forget）
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prevExec = sandbox.ksu.exec;
            sandbox.ksu.exec = function (cmd, args) {
                // 模拟 fire-and-forget：设置全局回调后异步给结果
                setTimeout(function () {
                    if (typeof sandbox.onExecResult === 'function') {
                        sandbox.onExecResult(0, '{"ok":true,"data":"1.0.0"}\n', '');
                    }
                }, 10);
                return undefined;
            };
            var settled = false;
            sandbox.FC.api.call('version').then(function (v) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                ok('global-cb 形态探测兼容', v === '1.0.0', 'got: ' + v);
                resolve();
            }, function (e) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                ok('global-cb 形态探测兼容', false, e.message);
                resolve();
            });
        });
    });

    // raw-script 形态：纯脚本路径不加 sh 前缀（单参数，区别于 direct 双参）
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prevExec = sandbox.ksu.exec;
            sandbox.ksu.exec = function (cmd, args) {
                // 只接受单参数纯脚本路径（无 args 数组、无 sh 前缀）
                var isRaw = typeof cmd === 'string' && args === undefined &&
                            cmd.indexOf('fcctl.sh') >= 0 && cmd.indexOf('/system/bin/sh') < 0;
                return Promise.resolve({ errno: 0, stdout: isRaw ? '{"ok":true,"data":"1.0.0"}\n' : '', stderr: '' });
            };
            var settled = false;
            sandbox.FC.api.call('version').then(function (v) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                ok('raw-script 形态探测兼容', v === '1.0.0', 'got: ' + v);
                resolve();
            }, function (e) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                ok('raw-script 形态探测兼容', false, e.message);
                resolve();
            });
        });
    });

    // delayed-read 形态：exec 后延迟扫描 window 新属性。
    // 注：vm 上下文的 for...in 对宿主侧设置的全局变量可见性有限制，
    // 此用例验证形态被探测到并出现在诊断里（真机上延迟扫描才有意义）。
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prevExec = sandbox.ksu.exec;
            sandbox.ksu.exec = function () { return undefined; };  // 全部形态失败
            var settled = false;
            sandbox.FC.api.call('version').then(function () {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                ok('delayed-read 形态探测兼容', false, '竟然成功了？');
                resolve();
            }, function (e) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                var msg = e.message || '';
                // 断言 delayed-read 形态出现在诊断里（被探测到）
                ok('delayed-read 形态探测兼容', msg.indexOf('delayed-read:') >= 0,
                   msg.slice(0, 150));
                resolve();
            });
        });
    });

    // obj-param 形态：ksu.exec({command,args,timeout,onStdout,...}) 对象参数
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prevExec = sandbox.ksu.exec;
            sandbox.ksu.exec = function (cmd, args) {
                // 只接受对象参数（command 字段）
                if (typeof cmd === 'object' && cmd !== null && cmd.command) {
                    setTimeout(function () {
                        if (typeof cmd.onStdout === 'function') {
                            cmd.onStdout('{"ok":true,"data":"1.0.0"}\n');
                        }
                    }, 10);
                    return undefined;
                }
                return Promise.resolve({ errno: 2, stdout: '', stderr: 'unsupported' });
            };
            var settled = false;
            sandbox.FC.api.call('version').then(function (v) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                ok('obj-param 形态探测兼容', v === '1.0.0', 'got: ' + v);
                resolve();
            }, function (e) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                ok('obj-param 形态探测兼容', false, e.message);
                resolve();
            });
        });
    });

    // sh-c 形态：ksu.exec("sh -c '...'") 显式 sh -c 包装
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prevExec = sandbox.ksu.exec;
            sandbox.ksu.exec = function (cmd) {
                var isShC = typeof cmd === 'string' && cmd.indexOf("sh -c '") >= 0 && cmd.indexOf('fcctl.sh') >= 0;
                return Promise.resolve({ errno: 0, stdout: isShC ? '{"ok":true,"data":"1.0.0"}\n' : '', stderr: '' });
            };
            var settled = false;
            sandbox.FC.api.call('version').then(function (v) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                ok('sh-c 形态探测兼容', v === '1.0.0', 'got: ' + v);
                resolve();
            }, function (e) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                ok('sh-c 形态探测兼容', false, e.message);
                resolve();
            });
        });
    });

    // file-fetch 形态：ksu.exec 重定向到 webroot/.fc_out + 唯一结束标记，
    // fetch() 读取（v1.0.15 起轮询只有见到本请求标记才认账）。
    // 本用例模拟真机竞态：第 1 次 fetch 返回陈旧内容（上个命令的残留，
    // 绝不含本请求标记——旧实现会误当结果，导致「不是合法 JSON」），
    // 第 2 次起返回本命令完成后的内容（输出 + 唯一结束标记行）。
    chain = chain.then(function () {
        return new Promise(function (resolve) {
            reloadBridge();
            var prevExec = sandbox.ksu.exec;
            var prevFetch = sandbox.fetch;
            var reqId = '';
            var fetchCount = 0;
            sandbox.ksu.exec = function (cmd) {
                // fire-and-forget：从命令行提取本请求的结束标记
                var m = /echo (__FCDONE_\w+__) >>/.exec(String(cmd));
                if (m) reqId = m[1];
                return undefined;
            };
            sandbox.fetch = function (url) {
                fetchCount++;
                var text;
                if (fetchCount === 1) {
                    // 陈旧内容：无本请求标记，必须被拒绝
                    text = '{"ok":true,"data":"STALE-OLD-CMD"}\n';
                } else {
                    // 本命令完成：输出 + 唯一结束标记
                    text = '{"ok":true,"data":"1.0.0"}\n' + reqId + '\n';
                }
                return Promise.resolve({
                    text: function () { return Promise.resolve(text); }
                });
            };
            var settled = false;
            sandbox.FC.api.call('version').then(function (v) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                sandbox.fetch = prevFetch;
                // 断言：拒绝第 1 次陈旧内容（fetch 超过 1 次）且最终取到正确输出
                ok('file-fetch 标记协议（拒绝陈旧内容）',
                   v === '1.0.0' && fetchCount >= 2,
                   'got: ' + v + '，fetch 次数: ' + fetchCount);
                resolve();
            }, function (e) {
                if (settled) return;
                settled = true;
                sandbox.ksu.exec = prevExec;
                sandbox.fetch = prevFetch;
                ok('file-fetch 标记协议（拒绝陈旧内容）', false, e.message);
                resolve();
            });
        });
    });

    chain.then(function () {
        sandbox.ksu.exec = oldExec0;
        reloadBridge();  // 重置形态探测状态，避免后续用例沿用上一形态
    });
    return chain;
    };
})();
ok('api 就绪', !!(sandbox.FC && sandbox.FC.api));
ok('app 就绪', !!(sandbox.FC && sandbox.FC.app && typeof sandbox.FC.app.boot === 'function'));
ok('四个页面就绪', !!(sandbox.FC && sandbox.FC.pages &&
  sandbox.FC.pages.overview && sandbox.FC.pages.apps &&
  sandbox.FC.pages.settings && sandbox.FC.pages.diag));

console.log('[js-smoke] 执行 boot…');
try {
  sandbox.FC.app.boot();
  ok('boot 未抛异常', true);
} catch (e) {
  ok('boot 未抛异常', false, e.message);
}

console.log('[js-smoke] 逐页 render + load…');
const routes = ['overview', 'apps', 'settings', 'diag'];
routes.forEach(function (r) {
  try {
    sandbox.FC.app.renderRoute(r, false);
    ok('渲染 ' + r, true);
  } catch (e) {
    ok('渲染 ' + r, false, e.message);
  }
});

// 等异步落定后检查界面被真实数据填充；页面断言完成后再跑形态探测链
setTimeout(function () {
  sandbox.FC.api.call('status').then(function () {
    ok('总览填充了前台应用', viewEl.children.length > 0);
    const pkgEl = registry.get('ov-pkg');
    ok('ov-pkg 有内容', !!pkgEl && pkgEl.textContent.length > 0, pkgEl && pkgEl.textContent);
    const capsEl = registry.get('ov-caps');
    ok('能力徽标已渲染', !!capsEl && capsEl.children.length > 0);
    if (errbarText.textContent) {
      console.log('  [错误条内容] ' + errbarText.textContent);
    }

    ok('无 root 通道时有判定', typeof sandbox.FC.bridge.hasRoot() === 'boolean');

    // 页面用例的异步链已落定，此时跑形态探测链（mock 不再被交叉换掉）
    runFormProbeTests().then(function () {
      console.log('----------------------------------------');
      console.log('[js-smoke] 通过 ' + pass + ' 项，失败 ' + fail + ' 项');
      if (fail > 0) process.exit(1);
      console.log('[js-smoke] ALL PASS');
    });
  }).catch(function (e) {
    console.log('  [js-smoke] status 调用失败: ' + e.message);
    process.exit(1);
  });
}, 2500);
