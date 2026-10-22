/* ============================================================
   一个调度 · 设置页
   常用开关（≤8 项，带说明）+ 折叠高级参数（每项一行说明）
   ============================================================ */
(function (global) {
  var FC = global.FC || (global.FC = {});
  var el = FC.util.el;

  // [键, 标签, 说明, 类型] 类型: switch / number / text / select
  var COMMON = [
    ['enabled', '总开关', '关闭后还原全部节点，守护进程待命', 'switch'],
    ['fps.enable', '稳帧', '游戏内按卡顿率阶梯调整最低频（带滞回防震荡）', 'switch'],
    ['thermal.enable', '温控联动', '温度超阈值时封顶频率上限；只封顶，永不停温控服务', 'switch'],
    ['feature.cpu', 'CPU 调频', '是否允许改写 CPU 频率节点', 'switch'],
    ['feature.cpuset', 'cpuset', '是否允许调整后台/前台 cpuset', 'switch'],
    ['feature.threads', '线程绑核', '是否允许把关键线程绑到大核', 'switch'],
    ['feature.io', 'IO 调优', '是否允许调整块设备 read_ahead_kb', 'switch'],
    ['feature.mem', '内存调优', '是否允许调整 swappiness', 'switch']
  ];

  var ADVANCED = [
    ['poll_interval_ms', '轮询周期（毫秒）', '前台识别间隔；越小响应越快，开销略增', 'number'],
    ['poll_idle_ms', '空闲退避（毫秒）', '连续无切换时放宽轮询，省电', 'number'],
    ['switch_debounce', '切换去抖次数', '同一目标连续命中这么多次才算切换，防抖', 'number'],
    ['switch_cooldown_ms', '切换冷却（毫秒）', '一次切换后多久内不再切换', 'number'],
    ['default_family', '默认档位', '未配置应用的亮屏档位', 'select', [['daily', '日用'], ['daily-plus', '日用增强']]],
    ['live_push_ms', '快照周期（毫秒）', '面板实时数据的落盘间隔', 'number'],
    ['fps.sample_ms', '稳帧采样（毫秒）', '帧统计采样间隔', 'number'],
    ['fps.jank_threshold', '卡顿率阈值（%）', '超过则升一档', 'number'],
    ['fps.hold_ms', '稳帧保持（毫秒）', '升档后至少保持这么久才允许降档（滞回）', 'number'],
    ['fps.steps.p0', '中核簇频率阶梯', '逗号分隔；第 1 个值为不干预档', 'text'],
    ['fps.steps.p1', '大核簇频率阶梯', '逗号分隔；第 1 个值为不干预档', 'text'],
    ['thermal.cap_temp', '封顶温度（毫度）', '如 48000 = 48℃', 'number'],
    ['thermal.recover_temp', '恢复温度（毫度）', '低于该温度解除封顶（滞回）', 'number'],
    ['thermal.cap.p0', '中核簇封顶频率', '超温时中核簇频率上限', 'number'],
    ['thermal.cap.p1', '大核簇封顶频率', '超温时大核簇频率上限', 'number']
  ];

  function field(kv, key, label, desc, type, options) {
    var wrap = el('div', 'fc-field');
    wrap.appendChild(el('label', 'fc-field-label', label + '（' + key + '）'));

    if (type === 'switch') {
      var row = el('div', 'fc-field-row');
      var sw = el('label', 'fc-switch');
      var input = el('input');
      input.type = 'checkbox';
      input.checked = (kv[key] === '1' || kv[key] === 'true');
      input.addEventListener('change', function () {
        FC.api.call('setsetting', key, input.checked ? '1' : '0')
          .catch(function (e) { FC.util.showError(e.message); });
      });
      sw.appendChild(input);
      sw.appendChild(el('span', 'fc-switch-slider'));
      row.appendChild(sw);
      var state = el('span', 'fc-field-desc', input.checked ? '已开启' : '已关闭');
      input.addEventListener('change', function () { state.textContent = input.checked ? '已开启' : '已关闭'; });
      row.appendChild(state);
      wrap.appendChild(row);
    } else {
      var row2 = el('div', 'fc-field-row');
      var ctrl;
      if (type === 'select') {
        ctrl = el('select', 'fc-select');
        (options || []).forEach(function (o) {
          var opt = el('option', null, o[1]);
          opt.value = o[0];
          ctrl.appendChild(opt);
        });
        ctrl.value = kv[key] || (options && options[0] ? options[0][0] : '');
      } else {
        ctrl = el('input', 'fc-input');
        if (type === 'number') ctrl.type = 'number';
        ctrl.value = kv[key] || '';
      }
      row2.appendChild(ctrl);
      var save = el('button', 'fc-btn fc-btn-ghost fc-btn-sm', '保存');
      save.addEventListener('click', function () {
        var v = String(ctrl.value || '').trim();
        if (!v) { FC.util.showError('值不能为空'); return; }
        FC.api.call('setsetting', key, v)
          .then(function () { FC.util.showError('已保存 ' + key + '=' + v); })
          .catch(function (e) { FC.util.showError(e.message); });
      });
      row2.appendChild(save);
      wrap.appendChild(row2);
    }
    wrap.appendChild(el('div', 'fc-field-desc', desc));
    return wrap;
  }

  function parseConf(text) {
    var kv = {};
    String(text || '').split('\n').forEach(function (line) {
      line = line.trim();
      if (!line || line[0] === '#') return;
      var eq = line.indexOf('=');
      if (eq > 0) kv[line.slice(0, eq)] = line.slice(eq + 1);
    });
    return kv;
  }

  var settingsPage = {
    render: function (view) {
      var frag = document.createDocumentFragment();

      var c1 = el('div', 'fc-card');
      c1.appendChild(el('div', 'fc-card-title', '常用开关'));
      COMMON.forEach(function (f) {
        c1.appendChild(field({}, f[0], f[1], f[2], f[3], f[4]));
      });
      frag.appendChild(c1);

      var c2 = el('div', 'fc-card');
      var det = el('details', 'fc-details');
      det.appendChild(el('summary', 'fc-details-summary', '高级参数（不推荐普通用户调整）'));
      ADVANCED.forEach(function (f) {
        det.appendChild(field({}, f[0], f[1], f[2], f[3], f[4]));
      });
      c2.appendChild(det);
      frag.appendChild(c2);

      var c3 = el('div', 'fc-card');
      c3.appendChild(el('div', 'fc-card-title', '配置备份'));
      c3.appendChild(el('div', 'fc-field-desc', '导出全部配置为文本，可留存；重装模块不会自动恢复，请手动保存'));
      var exp = el('button', 'fc-btn fc-btn-ghost', '导出配置');
      exp.addEventListener('click', function () {
        FC.api.call('export').then(function (b64) {
          var text = FC.api.b64decode(b64);
          var w = window.open('', '_blank');
          if (w) {
            w.document.write('<pre style="white-space:pre-wrap;word-break:break-all">' +
              text.replace(/&/g, '&amp;').replace(/</g, '&lt;') + '</pre>');
            w.document.close();
          } else {
            FC.util.showError('浏览器拦截了新窗口，请允许弹出后重试');
          }
        }).catch(function (e) { FC.util.showError(e.message); });
      });
      c3.appendChild(exp);
      frag.appendChild(c3);

      view.appendChild(frag);
    },

    load: function () {
      return FC.api.call('settings').then(function (b64) {
        var kv = parseConf(FC.api.b64decode(b64));
        // 用真实值重建表单
        var view = document.getElementById('fc-view');
        view.innerHTML = '';
        var frag = document.createDocumentFragment();
        var c1 = el('div', 'fc-card');
        c1.appendChild(el('div', 'fc-card-title', '常用开关'));
        COMMON.forEach(function (f) { c1.appendChild(field(kv, f[0], f[1], f[2], f[3], f[4])); });
        frag.appendChild(c1);
        var c2 = el('div', 'fc-card');
        var det = el('details', 'fc-details');
        det.appendChild(el('summary', 'fc-details-summary', '高级参数（不推荐普通用户调整）'));
        ADVANCED.forEach(function (f) { det.appendChild(field(kv, f[0], f[1], f[2], f[3], f[4])); });
        c2.appendChild(det);
        frag.appendChild(c2);
        view.appendChild(frag);
      });
    }
  };

  FC.pages = FC.pages || {};
  FC.pages.settings = settingsPage;
})(window);
