/* ============================================================
   一个调度 · api：信封解析与错误语义
   协议：fcctl.sh 永远输出单行 JSON
     {"ok":true,"data":...} / {"ok":false,"msg":"..."}
   ApiError 携带命令原文与耗时——出错时用户能看到是哪条命令、
   花了多久、返回什么，而不是一个干巴巴的「失败」。
   ============================================================ */
(function (global) {
  var FC = global.FC || (global.FC = {});

  function ApiError(msg, cmd, ms, raw) {
    this.name = 'ApiError';
    this.message = msg;
    this.cmd = cmd || '';
    this.ms = ms || 0;
    this.raw = raw || '';
  }
  ApiError.prototype = Object.create(Error.prototype);
  ApiError.prototype.constructor = ApiError;

  function parseEnvelope(stdout, cmd, ms) {
    // 信封行启发式：优先取最后一个以 { 开头的行（fcctl.sh 的 stderr 警告
    // 经 2>&1 可能排在信封之后，容忍此类混入）；兜底取最后一个非空行。
    // 信封永远是单行 JSON（data 走 base64），该启发式安全。
    var lines = String(stdout || '').split('\n').filter(function (l) {
      return l.trim().length > 0;
    });
    var line = null;
    for (var i = lines.length - 1; i >= 0; i--) {
      if (lines[i].trim().charAt(0) === '{') { line = lines[i]; break; }
    }
    if (!line && lines.length) line = lines[lines.length - 1];
    if (!line) {
      // 带当前 exec 形态 + API 真相摘要：刷入新版后用户截图即可判断
      // 「新 bridge 在跑但全形态失败」（带形态名/摘要）vs「旧页面被缓存」（都不带）。
      // 摘要含 ksu.exec 形参个数、对象键、各形态返回——一次截图匹配真实签名。
      var mode = FC.bridge.mode && FC.bridge.mode();
      var diag = FC.bridge.diag && FC.bridge.diag();
      throw new ApiError('设备端无输出（命令可能未执行' +
                         (mode ? '，exec 形态: ' + mode : '') +
                         (diag ? '；' + diag : '') + '）', cmd, ms, String(stdout || ''));
    }
    var obj = null;
    try {
      obj = JSON.parse(line);
    } catch (e) {
      throw new ApiError('设备端返回不是合法 JSON', cmd, ms, line.slice(0, 300));
    }
    if (obj && obj.ok === true) return obj.data;
    var msg = (obj && obj.msg) ? obj.msg : '未知错误';
    if (ms >= 11000) msg += '（耗时 ' + Math.round(ms / 100) / 10 + 's，可能超时）';
    throw new ApiError(msg, cmd, ms, line.slice(0, 300));
  }

  FC.api = {
    ApiError: ApiError,

    // call('status') -> Promise<data>；失败抛 ApiError（带命令与耗时）
    call: function () {
      var args = Array.prototype.slice.call(arguments);
      return FC.bridge.exec(args).then(function (r) {
        if (r.timed_out) {
          throw new ApiError('命令超时（' + Math.round(r.ms / 100) / 10 + 's）：' + r.cmd, r.cmd, r.ms);
        }
        if (r.thrown) {
          throw new ApiError('root 通道不可用：' + (r.stderr || '未知原因'), r.cmd, r.ms);
        }
        return parseEnvelope(r.stdout, r.cmd, r.ms);
      });
    },

    // callRaw：不过信封，直接拿原始结果（自检页需要退出码/耗时/原文）
    callRaw: function () {
      var args = Array.prototype.slice.call(arguments);
      return FC.bridge.exec(args).then(function (r) {
        if (r.timed_out) r.timed_out = true;
        return r;
      });
    },

    // base64 解码（浏览器内置 atob）
    b64decode: function (b64) {
      if (!b64) return '';
      try {
        var bin = atob(b64);
        var bytes = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
        return new TextDecoder('utf-8').decode(bytes);
      } catch (e) {
        return '';
      }
    },

    b64encode: function (text) {
      try {
        return btoa(unescape(encodeURIComponent(text)));
      } catch (e) {
        return '';
      }
    }
  };
})(window);
