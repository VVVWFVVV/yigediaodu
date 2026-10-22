/* ============================================================
   一个调度 · app：路由 / 骨架先行 / 页面级超时 / 串行轮询 / 错误条
   ============================================================ */
(function (global) {
  var FC = global.FC || (global.FC = {});
  var doc = document;

  // ---------------- 错误条 ----------------
  var errbar = null;
  var errTimer = null;
  function errorEl() {
    if (!errbar) {
      errbar = doc.getElementById('fc-errbar');
    }
    return errbar;
  }
  function showError(msg) {
    var bar = errorEl();
    if (!bar) return;
    var t = doc.getElementById('fc-errbar-text');
    if (t) t.textContent = msg;
    bar.style.display = 'flex';
    if (errTimer) clearTimeout(errTimer);
    errTimer = setTimeout(hideError, 10000);
  }
  function hideError() {
    var bar = errorEl();
    if (bar) bar.style.display = 'none';
  }
  FC.ui = { error: showError, clear: hideError };

  // ---------------- 小工具 ----------------
  function el(tag, cls, text) {
    var e = doc.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  }
  function esc(s) {
    return String(s === undefined || s === null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }
  function skel(n) {
    var f = doc.createDocumentFragment();
    for (var i = 0; i < n; i++) {
      f.appendChild(el('div', 'fc-skeleton fc-skeleton-block'));
    }
    return f;
  }
  FC.util = { el: el, esc: esc, skel: skel, showError: showError, hideError: hideError };

  // ---------------- 路由 ----------------
  var ROUTES = ['overview', 'apps', 'settings', 'diag'];
  var current = null;
  var pollTimer = null;
  var pollBusy = false;
  var pollChain = Promise.resolve();

  function setActiveTab(route) {
    var tabs = doc.querySelectorAll('.fc-tab');
    for (var i = 0; i < tabs.length; i++) {
      tabs[i].classList.toggle('is-active', tabs[i].getAttribute('data-route') === route);
    }
  }

  function stopPolling() {
    if (pollTimer) {
      clearTimeout(pollTimer);
      pollTimer = null;
    }
  }

  function schedulePoll(route) {
    stopPolling();
    if (route !== 'overview') return;
    // 串行化：上一次结束后才排下一次；飞行中不重复发起
    var tick = function () {
      if (current !== 'overview') return;
      if (pollBusy) {
        pollTimer = setTimeout(tick, 2000);
        return;
      }
      pollBusy = true;
      pollChain = pollChain.then(function () {
        var page = FC.pages && FC.pages[route];
        if (!page || !page.refresh) return null;
        return page.refresh();
      }).catch(function (e) {
        showError(e && e.message ? e.message : String(e));
      }).then(function () {
        pollBusy = false;
        if (current === 'overview') pollTimer = setTimeout(tick, 5000);
      });
    };
    pollTimer = setTimeout(tick, 5000);
  }

  function renderRoute(route, isInitial) {
    if (ROUTES.indexOf(route) < 0) route = 'overview';
    current = route;
    setActiveTab(route);

    var view = doc.getElementById('fc-view');
    var page = FC.pages && FC.pages[route];
    if (!page) {
      view.innerHTML = '<div class="fc-empty">页面模块缺失：' + esc(route) + '</div>';
      return;
    }

    // 骨架先行：先渲染布局，数据到达后原地填充
    view.innerHTML = '';
    try {
      page.render(view);
    } catch (e) {
      view.innerHTML = '<div class="fc-empty">页面渲染失败：' + esc(e && e.message) + '</div>';
      return;
    }
    view.setAttribute('data-loaded', '1');

    // 页面级加载：独立超时，任何单条命令卡住都不冻结界面
    // 单条命令最长 12s（bridge），页面允许串行多条，故放宽到 20s
    var settled = false;
    var pageTimeout = setTimeout(function () {
      if (settled) return;
      settled = true;
      showError('页面加载超时：' + route + '（可到自检页排查 root 通道）');
      if (page.onTimeout) page.onTimeout(view);
    }, 20000);

    Promise.resolve()
      .then(function () { return page.load(view); })
      .catch(function (e) {
        showError(e && e.message ? e.message : String(e));
        if (page.onError) page.onError(view, e);
      })
      .then(function () {
        if (settled) return;
        settled = true;
        clearTimeout(pageTimeout);
      });

    if (isInitial) {
      // 首屏只依赖最短命令；其余由各页面自行发起
    }
    schedulePoll(route);
  }

  function routeFromHash() {
    var h = (location.hash || '').replace(/^#\/?/, '');
    return ROUTES.indexOf(h) >= 0 ? h : 'overview';
  }

  function onHashChange() {
    renderRoute(routeFromHash(), false);
  }

  // ---------------- 顶栏 ----------------
  function initTopbar() {
    var sw = doc.getElementById('fc-enabled');
    if (sw) {
      sw.addEventListener('change', function () {
        var v = sw.checked ? '1' : '0';
        FC.api.call('enable', v).then(function () {
          showError(v === '1' ? '已恢复干预' : '已全局关闭，节点已还原');
        }).catch(function (e) { showError(e.message); });
      });
    }
    var close = doc.getElementById('fc-errbar-close');
    if (close) close.addEventListener('click', hideError);

    var tabs = doc.querySelectorAll('.fc-tab');
    for (var i = 0; i < tabs.length; i++) {
      (function (btn) {
        btn.addEventListener('click', function () {
          var r = btn.getAttribute('data-route');
          if (('#' + r) === location.hash) {
            renderRoute(r, false);
          } else {
            location.hash = r;
          }
        });
      })(tabs[i]);
    }
  }

  function refreshTopbar(status) {
    var vb = doc.getElementById('fc-version');
    if (vb && status && status.version) vb.textContent = 'v' + status.version;
  }

  // ---------------- 启动 ----------------
  FC.app = {
    boot: function () {
      initTopbar();
      global.addEventListener('hashchange', onHashChange);
      if (!FC.bridge.hasRoot()) {
        showError('未检测到 root 通道：请在 KernelSU 管理器中打开本面板');
      }
      renderRoute(routeFromHash(), true);

      // 版本号（最短命令，失败可容忍）
      FC.api.call('version').then(function (v) {
        var vb = doc.getElementById('fc-version');
        if (vb && v) vb.textContent = 'v' + String(v).replace(/^v/, '');
      }).catch(function () {});
    },
    renderRoute: renderRoute,
    refreshTopbar: refreshTopbar
  };
})(window);
