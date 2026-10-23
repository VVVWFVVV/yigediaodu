/* ============================================================
   一个调度 · pages：总览 + 应用
   总览首屏必须回答三件事：在管哪个应用 / 生效哪个档 / 干预是否真的生效
   ============================================================ */
(function (global) {
  var FC = global.FC || (global.FC = {});
  var el = FC.util.el;
  var esc = FC.util.esc;

  // 省电观察：滚动样本（面板本地收集，设备端只写不采）
  var liveHistory = [];
  var LIVE_HISTORY_MAX = 12;

  function capBadge(name, cn) {
    var cls = 'fc-cap fc-cap-off';
    var txt = '未开启';
    if (cn === 'ok') { cls = 'fc-cap fc-cap-ok'; txt = '已生效'; }
    else if (cn === 'degraded') { cls = 'fc-cap fc-cap-degraded'; txt = '已降级'; }
    else if (cn === 'fas') { cls = 'fc-cap fc-cap-ok'; txt = 'fas 接管'; }  // v1.0.17：频率维度交给帧感知调度
    var b = el('span', cls);
    b.innerHTML = '<b>' + esc(name) + '</b> · ' + esc(txt);
    return b;
  }

  function fmtFreq(khz) {
    if (!khz || isNaN(khz)) return '—';
    var mhz = (khz / 1000).toFixed(2);
    return mhz + '<small> GHz</small>';
  }

  // ---------------- 总览 ----------------
  var overview = {
    render: function (view) {
      var frag = document.createDocumentFragment();

      // 卡片 1：当前状态（三问之前两问）
      var c1 = el('div', 'fc-card');
      c1.appendChild(el('div', 'fc-card-title', '当前状态'));
      var r1 = el('div', 'fc-row');
      r1.appendChild(el('span', 'fc-row-label', '前台应用'));
      r1.appendChild(el('span', 'fc-row-value', '…'));
      r1.lastChild.id = 'ov-pkg';
      c1.appendChild(r1);
      var r2 = el('div', 'fc-row');
      r2.appendChild(el('span', 'fc-row-label', '生效档位'));
      r2.appendChild(el('span', 'fc-row-value', '…'));
      r2.lastChild.id = 'ov-profile';
      c1.appendChild(r2);
      var r3 = el('div', 'fc-row');
      r3.appendChild(el('span', 'fc-row-label', '守护进程'));
      r3.appendChild(el('span', 'fc-row-value', '…'));
      r3.lastChild.id = 'ov-daemon';
      c1.appendChild(r3);
      frag.appendChild(c1);

      // 卡片 2：档位切换（三个大按钮）
      var c2 = el('div', 'fc-card');
      c2.appendChild(el('div', 'fc-card-title', '档位切换'));
      c2.appendChild(el('div', 'fc-field-desc', '立即套用指定档位；随后前台场景变化时仍会自动调度'));
      var btns = el('div', 'fc-profile-btns');
      [['daily', '日用', '保守省电'], ['daily-plus', '日用增强', '抗卡顿'], ['game', '游戏', '加速稳帧']].forEach(function (p) {
        var b = el('button', 'fc-profile-btn');
        b.id = 'ov-btn-' + p[0];
        b.innerHTML = esc(p[1]) + '<small>' + esc(p[2]) + '</small>';
        b.addEventListener('click', function () {
          FC.api.call('apply', p[0]).then(function () {
            FC.util.showError('已套用：' + p[1]);
            overview.refresh();
          }).catch(function (e) { FC.util.showError(e.message); });
        });
        btns.appendChild(b);
      });
      c2.appendChild(btns);
      frag.appendChild(c2);

      // 卡片 3：干预是否生效（第三问）
      var c3 = el('div', 'fc-card');
      c3.appendChild(el('div', 'fc-card-title', '干预是否生效'));
      var caps = el('div', 'fc-cap-list');
      caps.id = 'ov-caps';
      c3.appendChild(caps);
      frag.appendChild(c3);

      // 卡片 4：实时
      var c4 = el('div', 'fc-card');
      c4.appendChild(el('div', 'fc-card-title', '实时'));
      var grid = el('div', 'fc-kv-grid');
      [['温度', 'ov-temp'], ['中核簇', 'ov-freq0'], ['大核簇', 'ov-freq1'], ['稳帧档', 'ov-fps'], ['温控', 'ov-therm'], ['帧感知', 'ov-fas']].forEach(function (k) {
        var kv = el('div', 'fc-kv');
        kv.appendChild(el('div', 'fc-kv-label', k[0]));
        var v = el('div', 'fc-kv-value', '…');
        v.id = k[1];
        kv.appendChild(v);
        grid.appendChild(kv);
      });
      c4.appendChild(grid);
      frag.appendChild(c4);

      // 卡片 5：省电观察
      var c5 = el('div', 'fc-card');
      c5.appendChild(el('div', 'fc-card-title', '省电观察'));
      c5.appendChild(el('div', 'fc-field-desc', '守护进程每 5 秒记录一次温度与频率；关闭模块干预前后各看几分钟，对比此处曲线即可判断省电效果'));
      var hist = el('div', 'fc-log');
      hist.id = 'ov-history';
      c5.appendChild(hist);
      frag.appendChild(c5);

      // 卡片 6：最近日志
      var c6 = el('div', 'fc-card');
      c6.appendChild(el('div', 'fc-card-title', '最近日志'));
      var logs = el('div', 'fc-log');
      logs.id = 'ov-logs';
      logs.textContent = '加载中…';
      c6.appendChild(logs);
      frag.appendChild(c6);

      view.appendChild(frag);
    },

    load: function (view) {
      return overview.refresh(view);
    },

    refresh: function () {
      var view = document.getElementById('fc-view');
      return FC.api.call('status').then(function (st) {
        FC.app.refreshTopbar(st);
        var pkg = document.getElementById('ov-pkg');
        var prof = document.getElementById('ov-profile');
        if (pkg) pkg.textContent = (st && st.pkg) ? st.pkg : '未识别';
        if (prof) prof.textContent = (st && st.profile) ? st.profile : '—';

        // 档位按钮高亮
        ['daily', 'daily-plus', 'game'].forEach(function (p) {
          var b = document.getElementById('ov-btn-' + p);
          if (b) b.classList.toggle('is-active', st && st.profile === p);
        });

        // 能力徽标
        var caps = document.getElementById('ov-caps');
        if (caps) {
          caps.innerHTML = '';
          var cn = (st && st.caps) || {};
          var names = { cpu: 'CPU', cpuset: 'cpuset', threads: '绑核', io: 'IO', mem: '内存', thermal: '温控', fps: '稳帧' };
          var any = false;
          for (var k in names) {
            if (!Object.prototype.hasOwnProperty.call(cn, k)) continue;
            any = true;
            caps.appendChild(capBadge(names[k], cn[k]));
          }
          if (!any) caps.textContent = '暂无能力状态（守护进程未落地过档位）';
        }

        // 守护状态
        return FC.api.call('daemon', 'status').then(function (d) {
          var dm = document.getElementById('ov-daemon');
          if (dm) dm.textContent = d && d.running ? ('运行中 (pid ' + d.pid + ')') : '未运行';
        }).catch(function () {
          var dm = document.getElementById('ov-daemon');
          if (dm) dm.textContent = '未知';
        });
      }).then(function () {
        // 实时快照
        return FC.api.call('live').then(function (lv) {
          lv = lv || {};
          var t = document.getElementById('ov-temp');
          var f0 = document.getElementById('ov-freq0');
          var f1 = document.getElementById('ov-freq1');
          var fps = document.getElementById('ov-fps');
          var th = document.getElementById('ov-therm');
          var fas = document.getElementById('ov-fas');
          if (t) t.innerHTML = (lv.TEMP || '—') + '<small> ℃</small>';
          var freqs = String(lv.FREQ || '').split(',');
          if (f0) f0.innerHTML = fmtFreq(freqs[0]);
          if (f1) f1.innerHTML = fmtFreq(freqs[1]);
          if (fps) fps.textContent = (lv.FPS_LEVEL || '0') + ' 档';
          if (th) th.textContent = lv.THERM === '1' ? '封顶中' : '正常';
          // v1.0.17：fas-rs 帧感知调度接管状态（接管中时显示当前模式）
          if (fas) {
            fas.textContent = lv.FAS === '1'
              ? ('接管中' + (lv.FASMODE && lv.FASMODE !== '-' ? ' · ' + lv.FASMODE : ''))
              : '未接管';
          }

          // 省电观察滚动记录
          if (lv.TEMP) {
            liveHistory.push({
              t: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
              temp: lv.TEMP,
              f0: freqs[0] ? (freqs[0] / 1000).toFixed(1) : '—',
              f1: freqs[1] ? (freqs[1] / 1000).toFixed(1) : '—'
            });
            while (liveHistory.length > LIVE_HISTORY_MAX) liveHistory.shift();
            var h = document.getElementById('ov-history');
            if (h) {
              var lines = liveHistory.map(function (s) {
                return s.t + '  ' + s.temp + '℃  中核 ' + s.f0 + 'GHz  大核 ' + s.f1 + 'GHz';
              });
              h.textContent = lines.join('\n');
            }
          }
        }).catch(function () {});
      }).then(function () {
        // 日志（次要数据，失败可容忍）
        return FC.api.call('logs', '15').then(function (b64) {
          var logs = document.getElementById('ov-logs');
          if (logs) {
            var text = FC.api.b64decode(b64);
            logs.textContent = text || '（暂无日志）';
          }
        }).catch(function () {
          var logs = document.getElementById('ov-logs');
          if (logs) logs.textContent = '（日志加载失败，不影响调度）';
        });
      }).catch(function (e) {
        throw e;
      });
    }
  };

  // ---------------- 应用 ----------------
  var MODES = [['daily', '日用'], ['daily-plus', '日用增强'], ['game', '游戏'], ['bg', '后台治理'], ['off', '不干预']];

  var appsPage = {
    render: function (view) {
      var frag = document.createDocumentFragment();

      var c1 = el('div', 'fc-card');
      c1.appendChild(el('div', 'fc-card-title', '按应用配置'));
      c1.appendChild(el('div', 'fc-field-desc', '为指定应用锁定档位；未配置的应用按场景自动调度（游戏走游戏档，其余走日用档）'));
      var list = el('div');
      list.id = 'app-list';
      list.appendChild(el('div', 'fc-skeleton fc-skeleton-block'));
      c1.appendChild(list);
      frag.appendChild(c1);

      var c2 = el('div', 'fc-card');
      c2.appendChild(el('div', 'fc-card-title', '添加应用'));
      var pkgInput = el('input', 'fc-input');
      pkgInput.id = 'app-new-pkg';
      pkgInput.placeholder = '包名，如 com.tencent.tmgp.sgame';
      c2.appendChild(pkgInput);
      var sel = el('select', 'fc-select');
      sel.id = 'app-new-mode';
      sel.style.marginTop = '8px';
      MODES.forEach(function (m) {
        var o = el('option', null, m[1]);
        o.value = m[0];
        sel.appendChild(o);
      });
      sel.value = 'game';
      c2.appendChild(sel);
      var add = el('button', 'fc-btn');
      add.style.marginTop = '10px';
      add.textContent = '添加';
      add.addEventListener('click', function () {
        var pkg = pkgInput.value.trim();
        if (!/^[A-Za-z0-9_.]+$/.test(pkg)) {
          FC.util.showError('包名格式不正确');
          return;
        }
        FC.api.call('mode', pkg, sel.value).then(function () {
          pkgInput.value = '';
          return appsPage.refresh();
        }).catch(function (e) { FC.util.showError(e.message); });
      });
      c2.appendChild(add);
      frag.appendChild(c2);

      view.appendChild(frag);
    },

    load: function () {
      return appsPage.refresh();
    },

    refresh: function () {
      return FC.api.call('list').then(function (arr) {
        var list = document.getElementById('app-list');
        if (!list) return;
        list.innerHTML = '';
        if (!arr || !arr.length) {
          list.appendChild(el('div', 'fc-empty', '暂无按应用配置'));
          return;
        }
        arr.forEach(function (app) {
          var row = el('div', 'fc-app');
          var name = el('div', 'fc-app-name', app.pkg);
          name.appendChild(el('small', null, '当前档位：' + app.mode +
            (app.fas === 1 ? ' · 帧感知接管' : '')));
          row.appendChild(name);

          var sel = el('select', 'fc-select');
          sel.style.width = 'auto';
          sel.style.flex = 'none';
          MODES.forEach(function (m) {
            var o = el('option', null, m[1]);
            o.value = m[0];
            sel.appendChild(o);
          });
          sel.value = app.mode;
          sel.addEventListener('change', function () {
            FC.api.call('mode', app.pkg, sel.value)
              .then(function () { return appsPage.refresh(); })
              .catch(function (e) { FC.util.showError(e.message); });
          });
          row.appendChild(sel);

          var del = el('button', 'fc-btn fc-btn-ghost fc-btn-sm', '删除');
          del.addEventListener('click', function () {
            FC.api.call('delapp', app.pkg)
              .then(function () { return appsPage.refresh(); })
              .catch(function (e) { FC.util.showError(e.message); });
          });
          row.appendChild(del);

          list.appendChild(row);
        });
      });
    }
  };

  FC.pages = FC.pages || {};
  FC.pages.overview = overview;
  FC.pages.apps = appsPage;
})(window);
