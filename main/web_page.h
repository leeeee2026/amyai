#pragma once

// 配网/设置页面（HTML + CSS + JS，全部嵌在固件 flash 里，不占 RAM）
//
// 单页面设计：页面里同时包含"Wi-Fi 配置"和"身体信息"两块，
// 加载时先问板子 /state，按状态只显示其中一块：
//   未配网 -> 只显示 Wi-Fi 配置
//   已配网 -> 只显示身体信息
//
// 好处：HTTP 只需要一个页面路由，不用为两种状态准备两份页面；
//       切换状态时只是 JS 切 display，体验更快。

static const char kWebPage[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>AmyAI 智能按摩椅 · 设置</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:0;background:#f2f4f8;color:#222;font-size:16px;line-height:1.5;
     font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:520px;margin:0 auto;padding:16px}
.head{text-align:center;padding:20px 0 12px}
.head h1{margin:0;font-size:21px;font-weight:600;color:#1a1a1a}
.head p{margin:6px 0 0;font-size:13px;color:#888}
.card{background:#fff;border-radius:14px;padding:16px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.06)}
.card h2{margin:0 0 12px;font-size:15px;font-weight:600;color:#333;display:flex;align-items:center}
.card h2 .bar{width:3px;height:15px;background:#4a7cff;border-radius:2px;margin-right:8px}
.row{display:flex;align-items:center;margin-bottom:12px}
.row:last-child{margin-bottom:0}
.row label{width:78px;flex:none;font-size:15px;color:#555}
.row input,.row select{flex:1;min-width:0;height:44px;padding:0 12px;font-size:16px;color:#222;
     border:1px solid #dfe3ea;border-radius:10px;background:#fafbfc;outline:none}
.row input:focus,.row select:focus{border-color:#4a7cff;background:#fff}
.unit{flex:none;margin-left:8px;font-size:13px;color:#999;width:20px}
.seg{display:flex;flex:1;gap:8px}
.seg button{flex:1;height:44px;font-size:15px;border:1px solid #dfe3ea;border-radius:10px;
     background:#fafbfc;color:#555}
.seg button.on{background:#4a7cff;border-color:#4a7cff;color:#fff;font-weight:600}
.btns{display:flex;gap:10px;margin-top:6px}
.btn{flex:1;height:50px;border:none;border-radius:12px;font-size:17px;font-weight:600;letter-spacing:1px}
.btn.primary{background:#4a7cff;color:#fff}
.btn.primary:active{background:#3a66dd}
.skip{font-size:12px;color:#aaa;text-align:center;margin-top:10px;line-height:1.6}
.msg{margin-top:14px;padding:12px;border-radius:10px;font-size:14px;display:none}
.msg.ok{display:block;background:#e8f7ee;color:#1a7f43}
.msg.err{display:block;background:#fdeaea;color:#b33}
.tips{font-size:12px;color:#999;margin-top:10px;line-height:1.7}
.scan{margin-left:8px;padding:0 12px;height:40px;flex:none;border:1px solid #dfe3ea;border-radius:10px;
     background:#fafbfc;font-size:13px;color:#555}
.state{font-size:12px;color:#1a7f43;background:#e8f7ee;border-radius:8px;padding:8px 10px;margin-bottom:12px}
.hide{display:none!important}
.loading{text-align:center;padding:40px 0;color:#999;font-size:14px}
</style>
</head>
<body>
<div class="wrap">

  <div class="head">
    <h1>AmyAI 智能按摩椅</h1>
    <p id="subtitle">设备设置</p>
  </div>

  <div class="loading" id="loading">正在读取设备状态…</div>

  <!-- ============ 区块 1：Wi-Fi 配置（未配网时显示） ============ -->
  <div id="secWifi" class="hide">
    <form id="fw" onsubmit="return saveWifi(event)">
      <div class="card">
        <h2><span class="bar"></span>Wi-Fi 连接</h2>
        <div class="row">
          <label>网络</label>
          <input type="text" id="ssid" placeholder="点右侧扫描或手动输入" autocomplete="off">
          <button type="button" class="scan" onclick="scan()">扫描</button>
        </div>
        <div class="row hide" id="listRow">
          <label>选择</label>
          <select id="list" onchange="pick()"></select>
        </div>
        <div class="row">
          <label>密码</label>
          <input type="password" id="pwd" placeholder="Wi-Fi 密码" autocomplete="off">
        </div>
      </div>
      <div class="btns">
        <button type="submit" class="btn primary" id="okWifi">保存并连接</button>
      </div>
      <p class="tips">保存后设备会立即连接该 Wi-Fi，连接成功会自动进入下一步（填写身体信息）。</p>
    </form>
  </div>

  <!-- ============ 区块 2：身体信息（已配网时显示） ============ -->
  <div id="secProfile" class="hide">
    <div class="state" id="wifiState">Wi-Fi 已连接</div>
    <form id="fp" onsubmit="return saveProfile(event)">
      <div class="card">
        <h2><span class="bar"></span>身体信息</h2>
        <div class="row">
          <label>身高</label>
          <input type="number" id="height" placeholder="选填" inputmode="numeric">
          <span class="unit">cm</span>
        </div>
        <div class="row">
          <label>体重</label>
          <input type="number" id="weight" placeholder="选填" inputmode="numeric">
          <span class="unit">kg</span>
        </div>
        <div class="row">
          <label>年龄</label>
          <input type="number" id="age" placeholder="选填" inputmode="numeric">
          <span class="unit">岁</span>
        </div>
        <div class="row">
          <label>性别</label>
          <div class="seg">
            <button type="button" id="g1" onclick="gender(1)">男</button>
            <button type="button" id="g2" onclick="gender(2)">女</button>
          </div>
        </div>
        <p class="skip">全部可以不填。填了以后会根据你的情况推荐按摩力度和时间</p>
      </div>
      <div class="btns">
        <button type="submit" class="btn primary" id="okProf">保存</button>
      </div>
      <p class="tips">保存后设备回到正常工作状态，需要重新喊唤醒词才能使用语音功能。</p>
    </form>
  </div>

  <div class="msg" id="msg"></div>
  <div id="dbg" style="display:none;margin-top:10px;padding:10px;border-radius:8px;
       background:#fff8e1;color:#8a6d3b;font-size:12px;line-height:1.6;word-break:break-all"></div>
</div>

<script>
var g = 0;
var provisioned = false;

// ---- 调试输出：任何异常都显示在页面上，方便排查 ----
function dbg(text) {
  var el = document.getElementById('dbg');
  if (!el) return;
  el.style.display = 'block';
  var line = document.createElement('div');
  line.textContent = text;
  el.appendChild(line);
}
window.onerror = function(m, s, l, c, e) { dbg('JS错误: ' + m + ' @' + l + ':' + c); return false; };
window.addEventListener('unhandledrejection', function(ev) { dbg('Promise错误: ' + ev.reason); });

// 带超时和详细报错的请求封装
function api(url, opt, timeoutMs) {
  timeoutMs = timeoutMs || 8000;
  var ctrl = new AbortController();
  var timer = setTimeout(function() { ctrl.abort(); }, timeoutMs);
  var o = opt || {};
  o.signal = ctrl.signal;
  return fetch(url, o).then(function(r) {
    clearTimeout(timer);
    if (!r.ok) { throw new Error('HTTP ' + r.status); }
    return r.text();
  }).then(function(txt) {
    try { return JSON.parse(txt); }
    catch (e) { throw new Error('返回不是JSON: ' + txt.slice(0, 120)); }
  }).catch(function(e) {
    clearTimeout(timer);
    dbg('请求 ' + url + ' 失败: ' + (e.name === 'AbortError' ? '超时' : e.message));
    throw e;
  });
}

function $(id) { return document.getElementById(id); }

function showMsg(text, ok) {
  var box = $('msg');
  box.className = 'msg ' + (ok ? 'ok' : 'err');
  box.style.display = 'block';   // ★ 必须显式设置，否则 hideMsg 的内联 none 会盖掉提示
  box.textContent = text;
}

function hideMsg() {
  var box = $('msg');
  box.className = 'msg';
  box.style.display = 'none';
}

function gender(v) {
  g = (g === v) ? 0 : v;
  $('g1').className = (g === 1) ? 'on' : '';
  $('g2').className = (g === 2) ? 'on' : '';
}

// 按设备状态显示对应区块
function showSection(hasWifi) {
  provisioned = hasWifi;
  $('loading').className = 'loading hide';
  $('secWifi').className = hasWifi ? 'hide' : '';
  $('secProfile').className = hasWifi ? '' : 'hide';
  $('subtitle').textContent = hasWifi ? '身体信息设置' : '第一步：连接家里的 Wi-Fi';
  if (hasWifi) { loadProfile(); }
}

function loadProfile() {
  api('/user', {}, 6000).then(function(p) {
    if (p.height) $('height').value = p.height;
    if (p.weight) $('weight').value = p.weight;
    if (p.age) $('age').value = p.age;
    if (p.gender) { g = p.gender; gender(p.gender); g = p.gender; }
    if (p.ssid) $('wifiState').textContent = 'Wi-Fi 已连接：' + p.ssid;
  }).catch(() => {});
}

// ---- Wi-Fi 扫描 ----
function scan() {
  showMsg('扫描中，请稍候…', true);
  api('/scan', {}, 10000).then(function(list) {
    var sel = $('list');
    sel.innerHTML = '';
    list.sort((a, b) => b.rssi - a.rssi);
    list.forEach(ap => {
      var o = document.createElement('option');
      o.value = ap.ssid;
      o.textContent = ap.ssid + (ap.rssi ? ' (' + ap.rssi + 'dBm)' : '');
      sel.appendChild(o);
    });
    $('listRow').className = list.length ? 'row' : 'row hide';
    // ★ 自动填入信号最强的那个，避免"看起来选了其实没选"导致保存时名称为空
    if (list.length) {
      $('ssid').value = list[0].ssid;
    }
    hideMsg();
  }).catch(function(e) { showMsg('扫描失败：' + e.message, false); });
}

function pick() {
  var v = $('list').value;
  if (v) $('ssid').value = v;
}

// ---- 提交 Wi-Fi：不重启，连上后自动切到身体信息 ----
function saveWifi(ev) {
  ev.preventDefault();
  var ssid = $('ssid').value.trim();
  var pwd  = $('pwd').value;
  if (!ssid) {
    showMsg('请先在上面选择或输入 Wi-Fi 名称', false);
    $('ssid').focus();
    return false;
  }
  $('okWifi').disabled = true;
  $('okWifi').textContent = '保存中…';
  showMsg('正在提交配置…', true);
  dbg('提交 Wi-Fi: ' + ssid);

  api('/save', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ssid: ssid, password: pwd})
  }).then(function(res) {
    if (!res || !res.ok) { throw new Error((res && res.error) || '保存失败'); }
    dbg('POST /save 成功');
    waitConnect();
  }).catch(function(e) {
    // POST 失败（部分手机在无外网环境会重置连接）→ 降级用 GET 再试一次
    dbg('POST 失败(' + e.message + ')，改用 GET 重试');
    showMsg('正在用兼容方式重试…', true);
    api('/save2?ssid=' + encodeURIComponent(ssid) + '&pwd=' + encodeURIComponent(pwd), {})
      .then(function(res) {
        if (!res || !res.ok) { throw new Error((res && res.error) || '保存失败'); }
        dbg('GET /save2 成功');
        waitConnect();
      })
      .catch(function(e2) {
        showMsg('保存失败：' + e2.message + '，请重试', false);
        $('okWifi').disabled = false;
        $('okWifi').textContent = '保存并连接';
      });
  });
  return false;
}

// 提交成功后轮询连接状态
function waitConnect() {
  showMsg('正在连接 Wi-Fi，请稍候…', true);
  var tries = 0;
  var timer = setInterval(function() {
    api('/state', {}, 5000).then(function(st) {
      if (st.connected) {
        clearInterval(timer);
        dbg('Wi-Fi 已连接，切换页面');
        showMsg('Wi-Fi 连接成功！请填写身体信息', true);
        setTimeout(function() { window.location.href = '/'; }, 800);
      } else if (st.failed) {
        clearInterval(timer);
        showMsg('连接失败：Wi-Fi 密码可能不对，请检查后重试', false);
        $('okWifi').disabled = false;
        $('okWifi').textContent = '保存并连接';
      }
    }).catch(function() {});
    if (++tries > 70) {   // 换信道时手机可能短暂断连，多等一会儿
      clearInterval(timer);
      showMsg('连接超时：请确认 Wi-Fi 信号良好后重试', false);
      $('okWifi').disabled = false;
      $('okWifi').textContent = '保存并连接';
    }
  }, 800);
}

// ---- 提交身体信息：保存后重启 ----
function saveProfile(ev) {
  ev.preventDefault();
  $('okProf').disabled = true;
  $('okProf').textContent = '保存中…';
  api('/save', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({
      height: parseInt($('height').value) || 0,
      weight: parseInt($('weight').value) || 0,
      age: parseInt($('age').value) || 0,
      gender: g
    })
  }).then(function(res) {
    if (!res.ok) throw new Error(res.error || '保存失败');
    showMsg('保存成功！设备即将重启回到工作状态，稍等几秒。', true);
    $('okProf').textContent = '已保存';
  }).catch(e => {
    showMsg('保存失败：' + e.message + '，请重试', false);
    $('okProf').disabled = false;
    $('okProf').textContent = '保存';
  });
  return false;
}

// 页面加载：问板子当前状态
api('/state', {}, 6000)
  .then(function(st) { showSection(!!st.provisioned); })
  .catch(function() {
    $('loading').textContent = '读取状态失败，请刷新重试（若一直失败请检查是否连着 AMYAI-CHAIR）';
  });
</script>
</body>
</html>)HTML";
