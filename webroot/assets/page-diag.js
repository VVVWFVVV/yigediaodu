/* ============================================================
   一个调度 · 自检页
   逐条探针：命令原文 / 耗时 / 退出码 / 输出原文（等宽可滚动）
   一键复制全部诊断文本——无需终端即可定位问题
   ============================================================ */
(function (global) {
  var FC = global.FC || (global.FC = {});
  var el = FC.util.el;
  var esc = FC.util.esc;

  var PROBES = [
    ['root 通道', ['version'], '最短命令：验证 ksu.exec 与模块目录可达'],
    ['运行状态', ['status'], '守护进程当前状态与能力生效情况'],
    ['前台识别', ['probe', 'fg'], '/proc 扫描结果（dumpsys 兜底时标注）'],
    ['亮灭屏', ['probe', 'screen'], '屏幕状态判定'],
    ['守护自检', ['selfcheck'], '二进制启动自检：配置/策略组/温度/命令通道'],
    ['实时快照', ['live'], '最近一次 live 推送内容'],
    ['系统体检', ['diag'], '设备/平台/守护/sched_ext/日志汇总']
  ];

  var results = [];

  function probeCard(p) {
    var card = el('div', 'fc-probe');
    card.setAttribute('data-name', p.name);

    var head = el('div', 'fc-probe-head');
    var name = el('span', 'fc-probe-name', p.name);
    var badge = el('span', 'fc-badge fc-badge-muted', '等待');
    badge.classList.add('fc-probe-badge');
    var time = el('span', 'fc-probe-time', '');
    head.appendChild(name);
    head.appendChild(badge);
    head.appendChild(time);

    var body = el('div', 'fc-probe-body');
    var cmd = el('div', 'fc-probe-cmd', 'fcctl.sh ' + p.args.join(' '));
    var out = el('div', 'fc-probe-out', '（尚未运行）');
    body.appendChild(cmd);
    body.appendChild(out);

    head.addEventListener('click', function () {
      card.classList.toggle('is-open');
    });

    card.appendChild(head);
    card.appendChild(body);
    return { card: card, badge: badge, time: time, out: out, name: p.name, desc: p.desc };
  }

  function renderResult(text) {
    // base64 -> 可读文本；否则原样
    if (/^[A-Za-z0-9+/=\r\n]+$/.test(text) && text.length > 40) {
      var d = FC.api.b64decode(text.trim());
      if (d) return d;
    }
    return text;
  }

  var diagPage = {
    render: function (view) {
      var frag = document.createDocumentFragment();

      var c0 = el('div', 'fc-card');
      c0.appendChild(el('div', 'fc-card-title', '一键自检'));
      c0.appendChild(el('div', 'fc-field-desc', '逐条执行探针（串行，避免压满设备端）。点击任一探针可展开命令原文与输出。'));
      var btns = el('div', 'fc-field-row');
      var run = el('button', 'fc-btn', '运行全部探针');
      run.id = 'diag-run';
      var copy = el('button', 'fc-btn fc-btn-ghost', '复制诊断文本');
      copy.id = 'diag-copy';
      btns.appendChild(run);
      btns.appendChild(copy);
      c0.appendChild(btns);
      frag.appendChild(c0);

      var c1 = el('div', 'fc-card');
      c1.id = 'diag-probes';
      PROBES.forEach(function (p) {
        var item = probeCard({ name: p[0], args: p[1], desc: p[2] });
        c1.appendChild(item.card);
      });
      frag.appendChild(c1);

      view.appendChild(frag);

      run.addEventListener('click', function () { diagPage.runAll(view); });
      copy.addEventListener('click', function () {
        var text = results.map(function (r) {
          return '===== ' + r.name + ' =====\n' + r.text + '\n';
        }).join('\n');
        if (!text) { FC.util.showError('请先运行探针'); return; }
        function done() { FC.util.showError('已复制 ' + results.length + ' 条诊断'); }
        if (navigator.clipboard && navigator.clipboard.writeText) {
          navigator.clipboard.writeText(text).then(done, function () { fallbackCopy(text); done(); });
        } else {
          fallbackCopy(text); done();
        }
      });
    },

    load: function () {
      // 自检页不自动跑：进入即跑也行，但会拉高首次负载；留给用户点「运行」
      return Promise.resolve();
    },

    runAll: function () {
      results = [];
      var cards = document.querySelectorAll('#diag-probes .fc-probe');
      var chain = Promise.resolve();
      PROBES.forEach(function (p, i) {
        chain = chain.then(function () {
          var card = cards[i];
          if (!card) return null;
          var badge = card.querySelector('.fc-probe-badge');
          var time = card.querySelector('.fc-probe-time');
          var out = card.querySelector('.fc-probe-out');
          badge.className = 'fc-badge fc-badge-muted fc-probe-badge';
          badge.textContent = '运行中';
          time.textContent = '';
          out.textContent = '…';
          card.classList.add('is-open');

          var started = Date.now();
          return FC.api.callRaw.apply(FC.api, p[1]).then(function (r) {
            var ms = Date.now() - started;
            time.textContent = ms + 'ms';
            var text;
            if (r.timed_out) {
              badge.className = 'fc-badge fc-badge-err fc-probe-badge';
              badge.textContent = '超时';
              text = '命令超时（' + Math.round(ms / 100) / 10 + 's）';
            } else if (r.thrown) {
              badge.className = 'fc-badge fc-badge-err fc-probe-badge';
              badge.textContent = '通道失败';
              text = r.stderr || 'root 通道不可用';
            } else {
              var ok = false;
              try {
                var line = String(r.stdout || '').trim().split('\n').pop();
                var obj = JSON.parse(line);
                ok = obj && obj.ok === true;
              } catch (e) { ok = false; }
              badge.className = 'fc-badge ' + (ok ? 'fc-badge-ok' : 'fc-badge-err') + ' fc-probe-badge';
              badge.textContent = ok ? '通过' : '失败';
              text = renderResult(String(r.stdout || ''));
              if (!text) text = '（无输出）';
            }
            out.textContent = text;
            results.push({ name: p[0], text: text });
            return null;
          });
        });
      });
      return chain;
    }
  };

  function fallbackCopy(text) {
    var ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand('copy'); } catch (e) {}
    document.body.removeChild(ta);
  }

  FC.pages = FC.pages || {};
  FC.pages.diag = diagPage;
})(window);
