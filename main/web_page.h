#pragma once

// 配网/设置页面（HTML + CSS + JS，全部嵌在固件 flash 里，不占 RAM）
//
// 单页面设计，左侧导航切换 6 个面板：
//   网络 / 身体 / 偏好 / 智能体 / 安全 / 高级
//
// 接口：
//   GET  /state   -> 配网状态，决定是否强制停在「网络」页
//   GET  /scan    -> 附近 Wi-Fi 列表
//   GET  /user    -> 身体信息 + 偏好设置（回填表单）
//   POST /save    -> 保存 Wi-Fi / 身体信息 / 偏好（偏好会同时下发主控）
//   GET  /reset   -> 恢复出厂（长按 3 秒触发）
//
// 偏好下发走 UART，和语音指令同一条通道，但不等 ACK（主控可能没接）。

static const char kWebPage[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>AmyAI 控制中心</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;padding:0;height:100%;overflow:hidden}
body{background:#0B0E13;color:#E8EDF7;font-size:15px;line-height:1.55;
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
body::before{content:"";position:fixed;inset:0;pointer-events:none;
  background:radial-gradient(560px 340px at 88% -8%,rgba(34,211,238,.10),transparent 70%),
             radial-gradient(520px 320px at -10% 108%,rgba(59,130,246,.10),transparent 70%)}

.rail{position:fixed;left:0;top:0;bottom:0;width:82px;z-index:60;background:rgba(15,19,27,.92);
  border-right:1px solid rgba(255,255,255,.055);backdrop-filter:blur(16px);
  display:flex;flex-direction:column;align-items:center;padding:14px 0 calc(14px + env(safe-area-inset-bottom))}
.rail .logo{width:40px;height:40px;border-radius:12px;flex:0 0 auto;margin-bottom:14px;
  background:linear-gradient(135deg,#22D3EE,#3B82F6);display:flex;align-items:center;justify-content:center;
  font-weight:700;font-size:17px;color:#06121A;box-shadow:0 6px 20px rgba(34,211,238,.32)}
.navs{flex:1;width:100%;display:flex;flex-direction:column;gap:3px;padding:0 9px;overflow-y:auto}
.navs::-webkit-scrollbar{width:0}
.nav{width:100%;border:none;background:transparent;cursor:pointer;display:flex;flex-direction:column;
  align-items:center;gap:3px;padding:8px 0;border-radius:12px;color:#7A8497;
  font-family:inherit;font-size:10px;letter-spacing:.3px;transition:all .2s;position:relative}
.nav svg{width:20px;height:20px;stroke:currentColor;fill:none;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round;transition:all .2s}
.nav.on{color:#EAF7FF;background:linear-gradient(135deg,rgba(34,211,238,.16),rgba(59,130,246,.16));
  box-shadow:inset 0 0 0 1px rgba(34,211,238,.3)}
.nav.on svg{stroke:#22D3EE;filter:drop-shadow(0 0 6px rgba(34,211,238,.55))}
.nav.on::after{content:"";position:absolute;left:-9px;top:50%;transform:translateY(-50%);
  width:3px;height:20px;border-radius:0 3px 3px 0;
  background:linear-gradient(180deg,#22D3EE,#3B82F6);box-shadow:0 0 10px rgba(34,211,238,.7)}

.main{position:absolute;left:82px;right:0;top:0;bottom:0;overflow-y:auto;-webkit-overflow-scrolling:touch;padding:0 16px 28px}
.main::-webkit-scrollbar{width:0}
.topbar,.pane{max-width:560px;margin-left:auto;margin-right:auto}
.topbar{padding:20px 0 6px}
.topbar h1{margin:0;font-size:19px;font-weight:600;letter-spacing:.3px;
  background:linear-gradient(90deg,#EAF7FF,#8FB7FF);-webkit-background-clip:text;background-clip:text;-webkit-text-fill-color:transparent}
.topbar .meta{display:flex;align-items:center;gap:7px;margin-top:7px;font-size:12px;color:#7A8497}
.dot{width:6px;height:6px;border-radius:50%;background:#34D399;box-shadow:0 0 0 3px rgba(52,211,153,.16)}
.dot.off{background:#F87171;box-shadow:0 0 0 3px rgba(248,113,113,.16)}
.chip{padding:1px 8px;border-radius:20px;font-size:10.5px;color:#67E8F9;
  background:rgba(34,211,238,.1);border:1px solid rgba(34,211,238,.28)}
.pane{display:none;animation:fade .26s ease}
.pane.on{display:block}
@keyframes fade{from{opacity:0;transform:translateY(7px)}to{opacity:1;transform:none}}

.card{background:linear-gradient(180deg,rgba(23,28,39,.96),rgba(19,23,32,.96));
  border:1px solid rgba(255,255,255,.06);border-radius:16px;padding:18px 15px 16px;margin:14px 0;
  position:relative;overflow:hidden}
.card::before{content:"";position:absolute;left:0;top:0;right:0;height:1px;
  background:linear-gradient(90deg,transparent,rgba(34,211,238,.4),transparent)}
.card h2{margin:0 0 15px;font-size:14.5px;font-weight:600;color:#CFE3F5;display:flex;align-items:center;gap:9px;letter-spacing:.4px}
.card h2 .bar{width:3px;height:14px;border-radius:2px;flex:0 0 3px;
  background:linear-gradient(180deg,#22D3EE,#3B82F6);box-shadow:0 0 8px rgba(34,211,238,.6)}
.sub{font-size:11.5px;color:#5F6879;margin:-6px 0 12px;letter-spacing:.3px}

.row{display:flex;align-items:center;gap:10px;margin-bottom:12px}
.row:last-of-type{margin-bottom:0}
.row>label{width:60px;flex:0 0 60px;font-size:14px;color:#8B96AB}
.row input[type=text],.row input[type=password],.row input[type=number]{
  flex:1 1 auto;min-width:0;height:46px;padding:0 13px;font-size:16px;color:#EAF2FF;
  background:rgba(9,12,18,.75);font-family:inherit;border:1px solid rgba(255,255,255,.075);
  border-radius:12px;outline:none;transition:all .18s}
.row input:focus{border-color:rgba(34,211,238,.55);background:rgba(9,12,18,.95);box-shadow:0 0 0 3px rgba(34,211,238,.1)}
.row input::placeholder{color:#4C5568}
.unit{flex:0 0 20px;font-size:11.5px;color:#5F6879}

/* 密码框：清空 + 显示切换 */
.pw{position:relative;flex:1 1 auto;min-width:0;display:flex;align-items:center}
.pw input[type=password],.pw input[type=text]{
  flex:1 1 auto;min-width:0;width:100%;height:46px;padding:0 78px 0 13px;font-size:16px;color:#EAF2FF;
  background:rgba(9,12,18,.75);font-family:inherit;border:1px solid rgba(255,255,255,.075);
  border-radius:12px;outline:none;transition:all .18s}
.pw input:focus{border-color:rgba(34,211,238,.55);background:rgba(9,12,18,.95);box-shadow:0 0 0 3px rgba(34,211,238,.1)}
.pw input::placeholder{color:#4C5568}
.pwb{position:absolute;top:50%;transform:translateY(-50%);width:34px;height:34px;border:none;
  background:transparent;color:#6F7A8E;cursor:pointer;display:flex;align-items:center;justify-content:center;
  border-radius:9px;font-family:inherit;font-size:13px;padding:0;transition:all .16s}
.pwb:active{background:rgba(34,211,238,.14);color:#7DE7F7}
.pwb.clr{right:42px}
.pwb.eye{right:6px}
.pwb svg{width:18px;height:18px;stroke:currentColor;fill:none;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}
.pwb.on{color:#22D3EE}

/* 卡片内的保存按钮 */
.card .btn.primary{height:46px;font-size:15px;margin-top:15px}
.seg{display:flex;gap:7px;flex:1 1 auto;min-width:0}
.seg button{flex:1 1 0;min-width:0;height:46px;font-size:14px;font-family:inherit;cursor:pointer;
  border:1px solid rgba(255,255,255,.075);border-radius:12px;background:rgba(9,12,18,.7);color:#8B96AB;transition:all .18s}
.seg button.on{background:linear-gradient(135deg,rgba(34,211,238,.2),rgba(59,130,246,.24));
  border-color:rgba(34,211,238,.55);color:#EAF7FF;font-weight:600;
  box-shadow:0 0 14px rgba(34,211,238,.18),inset 0 0 0 1px rgba(34,211,238,.2)}
.levels{display:flex;gap:6px;flex:1 1 auto;min-width:0}
.levels button{flex:1 1 0;min-width:0;height:44px;border-radius:11px;font-size:14px;font-family:inherit;cursor:pointer;
  border:1px solid rgba(255,255,255,.075);background:rgba(9,12,18,.7);color:#6F7A8E;transition:all .18s}
.levels button.on{background:linear-gradient(135deg,#22D3EE,#3B82F6);border-color:transparent;color:#04121A;
  font-weight:700;box-shadow:0 4px 16px rgba(34,211,238,.4)}
.chips{display:flex;flex-wrap:wrap;gap:7px;flex:1 1 auto}
.chips button{padding:8px 14px;border-radius:20px;font-size:13.5px;font-family:inherit;cursor:pointer;
  border:1px solid rgba(255,255,255,.075);background:rgba(9,12,18,.7);color:#8B96AB;transition:all .18s}
.chips button.on{background:rgba(34,211,238,.14);border-color:rgba(34,211,238,.5);color:#7DE7F7;font-weight:600}
.switch{position:relative;width:50px;height:29px;flex:0 0 50px;margin-left:auto}
.switch input{opacity:0;width:0;height:0;position:absolute}
.switch .track{position:absolute;inset:0;border-radius:20px;background:#252C3B;transition:all .22s;cursor:pointer}
.switch .track::after{content:"";position:absolute;width:23px;height:23px;border-radius:50%;
  background:#8892A6;top:3px;left:3px;transition:all .22s}
.switch input:checked + .track{background:linear-gradient(135deg,#22D3EE,#3B82F6);box-shadow:0 0 14px rgba(34,211,238,.4)}
.switch input:checked + .track::after{transform:translateX(21px);background:#fff}
.slider{flex:1 1 auto;min-width:0;display:flex;align-items:center;gap:9px}
.slider input[type=range]{flex:1 1 auto;min-width:0;-webkit-appearance:none;height:4px;border-radius:3px;outline:none;
  background:linear-gradient(90deg,#22D3EE var(--p,70%),#252C3B var(--p,70%))}
.slider input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;
  background:#0B0E13;border:2px solid #22D3EE;box-shadow:0 0 10px rgba(34,211,238,.7);cursor:pointer}
.slider .val{flex:0 0 42px;text-align:right;font-size:12.5px;color:#7DE7F7;font-weight:600;font-variant-numeric:tabular-nums}

.part{display:flex;align-items:center;gap:9px;margin-bottom:10px}.part:last-child{margin-bottom:0}
.part .pn{width:42px;flex:0 0 42px;font-size:13.5px;color:#8B96AB}
.mini{display:flex;gap:5px;flex:1 1 auto;min-width:0}
.mini button{flex:1 1 0;min-width:0;height:38px;border-radius:9px;font-size:12.5px;font-family:inherit;cursor:pointer;
  border:1px solid rgba(255,255,255,.07);background:rgba(9,12,18,.7);color:#6F7A8E;transition:all .18s}
.mini button.on{background:rgba(34,211,238,.16);border-color:rgba(34,211,238,.45);color:#7DE7F7;font-weight:600}

.btn{width:100%;height:48px;border-radius:13px;font-size:15.5px;font-family:inherit;border:none;cursor:pointer;transition:all .18s}
.btn.primary{background:linear-gradient(135deg,#22D3EE,#3B82F6);color:#04121A;font-weight:700;letter-spacing:1px;
  box-shadow:0 6px 22px rgba(34,211,238,.32)}
.btn.primary:active{transform:translateY(1px)}
.btn.scan{flex:0 0 68px;height:46px;border-radius:12px;cursor:pointer;font-size:13.5px;font-family:inherit;
  background:rgba(34,211,238,.1);border:1px solid rgba(34,211,238,.35);color:#7DE7F7}
.btn.danger{background:rgba(248,113,113,.08);border:1px solid rgba(248,113,113,.45);color:#FCA5A5;font-weight:600;
  position:relative;overflow:hidden}
.btn.danger.holding{background:rgba(248,113,113,.32);box-shadow:0 0 22px rgba(248,113,113,.35)}
.btn.soft{background:rgba(34,211,238,.1);border:1px solid rgba(34,211,238,.35);color:#7DE7F7;height:46px}
.tip{margin:11px 0 0;font-size:11.5px;color:#6E7A90;line-height:1.55}

/* 扫描结果列表 */
.aplist{max-height:0;overflow:hidden;transition:max-height .3s ease;border-radius:12px;
  background:rgba(9,12,18,.75);margin-bottom:0}
.aplist.open{max-height:300px;overflow-y:auto;border:1px solid rgba(255,255,255,.075);margin-bottom:12px}
.aplist::-webkit-scrollbar{width:0}
.ap{display:flex;align-items:center;gap:10px;padding:13px;cursor:pointer;
  border-bottom:1px solid rgba(255,255,255,.04);transition:background .15s}
.ap:last-child{border-bottom:none}
.ap:active{background:rgba(34,211,238,.1)}
.ap .nm{flex:1 1 auto;min-width:0;font-size:14.5px;color:#D5E3F2;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.ap .sg{flex:0 0 auto;font-size:11px;color:#6F7A8E;font-variant-numeric:tabular-nums}
.ap .cur{font-size:10px;color:#34D399;border:1px solid rgba(52,211,153,.4);
  border-radius:20px;padding:0 6px;margin-left:4px}
.bars{display:flex;align-items:flex-end;gap:2px;height:14px;flex:0 0 auto}
.bars i{width:3px;border-radius:1px;background:#333B4D}
.bars i.on{background:#22D3EE;box-shadow:0 0 4px rgba(34,211,238,.6)}
.sect{margin-top:20px}
.sect .line{display:flex;align-items:center;gap:10px;margin:0 0 12px}
.sect .line::before,.sect .line::after{content:"";flex:1;height:1px;background:rgba(255,255,255,.06)}
.sect .line span{font-size:10.5px;color:#4C5568;letter-spacing:2px}

.footer{position:fixed;left:82px;right:0;bottom:0;z-index:55;padding:10px 16px calc(10px + env(safe-area-inset-bottom));
  background:linear-gradient(180deg,rgba(11,14,19,.4),rgba(11,14,19,.96) 45%);backdrop-filter:blur(12px)}
.footer .in{max-width:560px;margin:0 auto}
.toast{position:fixed;left:calc(50% + 41px);bottom:36px;transform:translate(-50%,14px);
  background:rgba(34,211,238,.14);border:1px solid rgba(34,211,238,.45);color:#CDF6FF;font-size:13.5px;
  padding:10px 20px;border-radius:24px;opacity:0;pointer-events:none;transition:all .26s;z-index:99;
  backdrop-filter:blur(8px);box-shadow:0 8px 28px rgba(0,0,0,.5);text-align:center;max-width:80%}
.toast.show{opacity:1;transform:translate(-50%,0)}

@media (max-width:400px){
  .rail{width:72px}
  .main{left:72px;padding:0 13px 104px}
  .footer{left:72px;padding-left:13px;padding-right:13px}
  .toast{left:calc(50% + 36px)}
  .row>label{width:54px;flex:0 0 54px;font-size:13.5px}
  .card{padding:16px 13px 14px;border-radius:15px}
  .nav{font-size:9.5px;padding:7px 0}
  .nav svg{width:19px;height:19px}
}
</style>
</head>
<body>

<aside class="rail">
  <div class="logo">A</div>
  <div class="navs">
    <button class="nav on" onclick="go('net',this)">
      <svg viewBox="0 0 24 24"><path d="M5 12.5a10 10 0 0 1 14 0"/><path d="M8.5 16a5.5 5.5 0 0 1 7 0"/><circle cx="12" cy="19.5" r="1.2"/><path d="M2 9a14 14 0 0 1 20 0"/></svg>
      <span>网络</span></button>
    <button class="nav" onclick="go('body',this)">
      <svg viewBox="0 0 24 24"><circle cx="12" cy="7.5" r="3.5"/><path d="M4.5 20.5c0-4.1 3.4-7.5 7.5-7.5s7.5 3.4 7.5 7.5"/></svg>
      <span>身体</span></button>
    <button class="nav" onclick="go('pref',this)">
      <svg viewBox="0 0 24 24"><path d="M4 7h9M17 7h3M4 17h3M11 17h9"/><circle cx="15" cy="7" r="2.2"/><circle cx="8" cy="17" r="2.2"/></svg>
      <span>偏好</span></button>
    <button class="nav" onclick="go('agent',this)">
      <svg viewBox="0 0 24 24"><rect x="4.5" y="8" width="15" height="11" rx="3"/><circle cx="9.5" cy="13" r="1.3"/><circle cx="14.5" cy="13" r="1.3"/><path d="M12 8V4.5M12 4.5h2"/></svg>
      <span>智能体</span></button>
    <button class="nav" onclick="go('safe',this)">
      <svg viewBox="0 0 24 24"><path d="M12 3l7.5 3v6c0 4.5-3.2 7.8-7.5 9-4.3-1.2-7.5-4.5-7.5-9V6z"/><path d="M9.2 12.2l2 2 3.6-3.8"/></svg>
      <span>安全</span></button>
    <button class="nav" onclick="go('adv',this)">
      <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M12 2.8v2.4M12 18.8v2.4M4.6 7.4l2 1.2M17.4 15.4l2 1.2M4.6 16.6l2-1.2M17.4 8.6l2-1.2"/></svg>
      <span>高级</span></button>
  </div>
</aside>

<div class="main" id="main">
  <div class="topbar">
    <h1 id="title">网络连接</h1>
    <div class="meta"><span class="dot" id="stDot"></span><span id="stText">正在读取状态…</span><span class="chip" id="verChip">v1.2.3</span></div>
  </div>

  <!-- 网络 -->
  <section class="pane on" id="pane-net">
    <div class="card">
      <h2><span class="bar"></span>Wi-Fi 连接</h2>

      <!-- 视图 A：已连接 -->
      <div id="wifiView">
        <div class="row"><label>当前</label><input type="text" id="curSsid" readonly style="color:#7DE7F7"></div>
        <div class="row"><label>信号</label><input type="text" id="curRssi" readonly style="color:#7A8497"></div>
        <button class="btn soft" style="margin-top:15px" onclick="enterProv()">重新配置 Wi-Fi</button>
        <p class="tip">点上方按钮会暂停语音进入配网模式，配好后自动恢复。</p>
      </div>

      <!-- 视图 B：配网表单 -->
      <div id="wifiForm" style="display:none">
        <div class="row"><label>网络</label>
          <input type="text" id="ssid" placeholder="点扫描或手动输入" autocomplete="off">
          <button class="btn scan" id="scanBtn" onclick="scan()">扫描</button></div>
        <div class="aplist" id="apPanel"></div>
        <div class="row"><label>密码</label>
          <div class="pw">
            <input type="password" id="pwd" placeholder="Wi-Fi 密码" autocomplete="off">
            <button class="pwb clr" type="button" onclick="clearPwd()">✕</button>
            <button class="pwb eye" type="button" id="eyeBtn" onclick="togglePwd(this)">
              <svg viewBox="0 0 24 24"><path d="M2 12s3.8-6.5 10-6.5S22 12 22 12s-3.8 6.5-10 6.5S2 12 2 12z"/><circle cx="12" cy="12" r="2.8"/></svg>
            </button>
          </div>
        </div>
        <p class="tip" id="netTip">选择家里的 Wi-Fi 并输入密码。</p>
        <button class="btn primary" onclick="saveWifi()">连 接</button>
        <button class="btn" style="margin-top:10px;background:transparent;color:#7A8497;border:1px solid rgba(255,255,255,.08)"
                onclick="exitProv()">取消</button>
      </div>
    </div>
  </section>

  <!-- 身体 -->
  <section class="pane" id="pane-body">
    <div class="card">
      <h2><span class="bar"></span>基本信息</h2>
      <div class="row"><label>身高</label><input type="number" id="height" inputmode="numeric" placeholder="175"><span class="unit">cm</span></div>
      <div class="row"><label>体重</label><input type="number" id="weight" inputmode="numeric" placeholder="70"><span class="unit">kg</span></div>
      <div class="row"><label>年龄</label><input type="number" id="age" inputmode="numeric" placeholder="35"><span class="unit">岁</span></div>
      <div class="row"><label>性别</label>
        <div class="seg" id="gender"><button onclick="pick(this)">男</button><button onclick="pick(this)">女</button></div></div>
      <p class="tip">用于自动匹配按摩力度与程序；年龄与体重影响安全上限。可不填。</p>
      <button class="btn primary" onclick="savePrefs()">保 存</button>
    </div>
  </section>

  <!-- 偏好 -->
  <section class="pane" id="pane-pref">
    <div class="card">
      <h2><span class="bar"></span>默认程序</h2>
      <div class="chips" id="programs">
        <button onclick="pick(this)">全身放松</button><button onclick="pick(this)">肩颈</button>
        <button onclick="pick(this)">腰臀</button><button onclick="pick(this)">睡眠</button>
        <button onclick="pick(this)">舒展拉伸</button><button onclick="pick(this)">运动恢复</button>
      </div>
      <p class="tip">开机默认执行的程序。语音说「按肩颈」可临时切换。</p>
    </div>

    <div class="card">
      <h2><span class="bar"></span>力度与速度</h2>
      <div class="row"><label>力度</label>
        <div class="levels" id="strength">
          <button onclick="pick(this)">1</button><button onclick="pick(this)">2</button>
          <button onclick="pick(this)">3</button><button onclick="pick(this)">4</button><button onclick="pick(this)">5</button>
        </div></div>
      <div class="row"><label>速度</label>
        <div class="seg" id="speed"><button onclick="pick(this)">慢</button><button onclick="pick(this)">中</button><button onclick="pick(this)">快</button></div></div>
      <div class="row"><label>时长</label>
        <div class="seg" id="duration"><button onclick="pick(this)">10</button><button onclick="pick(this)">20</button><button onclick="pick(this)">30</button><button onclick="pick(this)">45</button></div></div>
      <p class="tip">力度 1–5 对应机芯伸缩深度；65 岁以上建议不超过 3 档。</p>
    </div>

    <div class="card">
      <h2><span class="bar"></span>按摩部位</h2>
      <div class="chips" id="parts">
        <button onclick="tog(this)">颈</button><button onclick="tog(this)">肩</button><button onclick="tog(this)">背</button>
        <button onclick="tog(this)">腰</button><button onclick="tog(this)">臀</button><button onclick="tog(this)">腿</button><button onclick="tog(this)">足</button>
      </div>
      <p class="tip">可多选。机芯只走选中区域对应的导轨段。</p>
    </div>

    <div class="card">
      <h2><span class="bar"></span>气囊强度</h2>
      <p class="sub">分部位独立调节</p>
      <div class="part"><span class="pn">颈</span><div class="mini" id="airNeck"><button onclick="pick(this)">弱</button><button onclick="pick(this)">中</button><button onclick="pick(this)">强</button></div></div>
      <div class="part"><span class="pn">肩</span><div class="mini" id="airShoulder"><button onclick="pick(this)">弱</button><button onclick="pick(this)">中</button><button onclick="pick(this)">强</button></div></div>
      <div class="part"><span class="pn">臂</span><div class="mini" id="airArm"><button onclick="pick(this)">弱</button><button onclick="pick(this)">中</button><button onclick="pick(this)">强</button></div></div>
      <div class="part"><span class="pn">腿</span><div class="mini" id="airLeg"><button onclick="pick(this)">弱</button><button onclick="pick(this)">中</button><button onclick="pick(this)">强</button></div></div>
      <div class="part"><span class="pn">足</span><div class="mini" id="airFoot"><button onclick="pick(this)">弱</button><button onclick="pick(this)">中</button><button onclick="pick(this)">强</button></div></div>
    </div>

    <div class="card">
      <h2><span class="bar"></span>足底</h2>
      <div class="row"><label>滚轮</label>
        <div class="seg" id="roller"><button onclick="pick(this)">关</button><button onclick="pick(this)">低</button><button onclick="pick(this)">中</button><button onclick="pick(this)">高</button></div></div>
    </div>

    <div class="card">
      <h2><span class="bar"></span>热敷</h2>
      <p class="sub">部位与温度独立设置</p>
      <div class="chips" id="heat" style="margin-bottom:14px">
        <button onclick="tog(this)">腰背</button><button onclick="tog(this)">腿</button>
        <button onclick="tog(this)">足</button><button onclick="tog(this)">手</button><button onclick="tog(this)">颈</button>
      </div>
      <div class="part"><span class="pn">温度</span><div class="mini" id="heatTemp"><button onclick="pick(this)">40℃</button><button onclick="pick(this)">45℃</button><button onclick="pick(this)">50℃</button></div></div>
      <p class="tip">恒温控制，达到设定温度后自动保温。皮肤敏感者建议 40℃。</p>
      <button class="btn primary" onclick="savePrefs()">保 存</button>
    </div>
  </section>

  <!-- 智能体 -->
  <section class="pane" id="pane-agent">
    <div class="card">
      <h2><span class="bar"></span>语音与对话</h2>
      <div class="row"><label>音量</label>
        <div class="slider"><input type="range" min="0" max="100" value="70" id="vol" oninput="volCh(this)"><span class="val" id="volv">70%</span></div></div>
      <div class="row"><label>语速</label>
        <div class="seg" id="ttsSpeed"><button onclick="pick(this)">慢</button><button onclick="pick(this)">标准</button><button onclick="pick(this)">快</button></div></div>
      <div class="row"><label>音色</label>
        <div class="seg" id="ttsVoice"><button onclick="pick(this)">女声</button><button onclick="pick(this)">温柔</button><button onclick="pick(this)">沉稳</button></div></div>
      <div class="row"><label>连聊</label>
        <div class="seg" id="follow"><button onclick="pick(this)">关</button><button onclick="pick(this)">15s</button>
          <button onclick="pick(this)">1m</button><button onclick="pick(this)">10m</button></div></div>
      <div class="row"><label>应答音</label><div class="switch"><input type="checkbox" id="ack"><span class="track"></span></div></div>
      <p class="tip">「连聊」开启后，回复完会继续听一段时间，不用重复喊唤醒词。</p>
      <button class="btn primary" onclick="savePrefs()">保 存</button>
    </div>
    <div class="card">
      <h2><span class="bar"></span>唤醒词</h2>
      <div class="row"><label>当前</label><input type="text" value="小鹿小鹿" readonly style="color:#7DE7F7"></div>
      <p class="tip">唤醒词随固件固定，更换需联系售后定制。</p>
    </div>
  </section>

  <!-- 安全 -->
  <section class="pane" id="pane-safe">
    <div class="card">
      <h2><span class="bar"></span>儿童锁</h2>
      <div class="row"><label>启用</label><div class="switch"><input type="checkbox" id="lock"><span class="track"></span></div></div>
      <p class="tip">开启后锁定面板与语音控制，只能通过手机解锁，防止儿童误操作。</p>
    </div>
    <div class="card">
      <h2><span class="bar"></span>久坐提醒</h2>
      <div class="row"><label>提醒</label>
        <div class="seg" id="sed"><button onclick="pick(this)">关</button><button onclick="pick(this)">30m</button>
          <button onclick="pick(this)">60m</button><button onclick="pick(this)">120m</button></div></div>
      <p class="tip">连续使用超过设定时长时，语音提醒起身活动。</p>
      <button class="btn primary" onclick="savePrefs()">保 存</button>
    </div>
    <div class="card">
      <h2><span class="bar" style="background:linear-gradient(180deg,#F87171,#FCA5A5)"></span>急停与保护</h2>
      <p class="tip" style="margin-top:0">
        · 长按机身「急停」键 2 秒，立即停止所有动作<br>
        · 检测到儿童、宠物进入机械行程时自动停机<br>
        · 单次按摩最长 45 分钟，到点自动回位<br>
        · 过流、过热、堵转均会触发保护并语音提示
      </p>
    </div>
  </section>

  <!-- 高级 -->
  <section class="pane" id="pane-adv">
    <div class="card">
      <h2><span class="bar"></span>固件升级</h2>
      <button class="btn soft" onclick="toast('已是最新版本')">检查更新</button>
      <p class="tip">连接 Wi-Fi 后可检查更新。</p>
    </div>
    <div class="sect"><div class="line"><span>危 险 操 作</span></div></div>
    <div class="card">
      <h2><span class="bar" style="background:linear-gradient(180deg,#F87171,#FCA5A5)"></span>恢复出厂设置</h2>
      <button class="btn danger" id="resetBtn"
        onmousedown="holdStart()" onmouseup="holdEnd()" onmouseleave="holdEnd()"
        ontouchstart="holdStart()" ontouchend="holdEnd()" ontouchcancel="holdEnd()">长按 3 秒清除所有设置</button>
      <p class="tip">将删除 Wi-Fi、身体信息、按摩偏好、智能体配置，设备会重启。</p>
    </div>
  </section>
</div>

<div class="toast" id="toast"></div>

<script>
var TITLES={net:'网络连接',body:'身体信息',pref:'按摩偏好',agent:'智能体',safe:'安全与保护',adv:'高级设置'};
var ST={provisioned:false};
function $(id){return document.getElementById(id)}
var tt=null;
function toast(m){var t=$('toast');t.textContent=m;t.classList.add('show');
  clearTimeout(tt);tt=setTimeout(function(){t.classList.remove('show')},1800)}
function go(p,el){
  var ns=document.querySelectorAll('.nav');
  for(var i=0;i<ns.length;i++){ns[i].classList.remove('on')}
  if(el){el.classList.add('on')}
  var ps=document.querySelectorAll('.pane');
  for(var j=0;j<ps.length;j++){ps[j].classList.remove('on')}
  $('pane-'+p).classList.add('on');
  $('title').textContent=TITLES[p];
  $('main').scrollTop=0;
}
function pick(el){
  var kids=el.parentNode.children;
  for(var i=0;i<kids.length;i++){kids[i].classList.remove('on')}
  el.classList.add('on');
}
function tog(el){el.classList.toggle('on')}
function idx(id){
  var kids=$(id).children;
  for(var i=0;i<kids.length;i++){if(kids[i].classList.contains('on'))return i}
  return 0;
}
function setIdx(id,i){
  var kids=$(id).children;
  for(var k=0;k<kids.length;k++){kids[k].classList.remove('on')}
  if(kids[i]){kids[i].classList.add('on')}
}
function mask(id){
  var m=0,kids=$(id).children;
  for(var i=0;i<kids.length;i++){if(kids[i].classList.contains('on'))m|=(1<<i)}
  return m;
}
function setMask(id,m){
  var kids=$(id).children;
  for(var i=0;i<kids.length;i++){
    if(m&(1<<i)){kids[i].classList.add('on')}else{kids[i].classList.remove('on')}
  }
}
function volCh(el){$('volv').textContent=el.value+'%';el.style.setProperty('--p',el.value+'%')}

// ---- Wi-Fi 卡两种视图：已连接 / 配网表单 ----
function renderWifi(){
  var showForm = !!ST.prov || !ST.connected;
  $('wifiView').style.display = showForm ? 'none' : 'block';
  $('wifiForm').style.display = showForm ? 'block' : 'none';
  if(ST.connected){
    $('curSsid').value = ST.ssid || '';
    $('curRssi').value = ST.rssi ? (ST.rssi + ' dBm') : '未知';
    $('ssid').value = ST.ssid || $('ssid').value;
  }
}
function provSet(on,cb){
  fetch('/prov',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({on:on})})
    .then(function(){if(cb)cb()}).catch(function(){if(cb)cb()});
}
function enterProv(){
  toast('已进入配网模式，语音暂停');
  provSet(true,function(){ST.prov=true;renderWifi();$('ssid').focus()});
}
function exitProv(){
  provSet(false,function(){ST.prov=false;renderWifi();toast('已退出配网模式，语音恢复')});
  setTimeout(load,800);
}

function esc(s){
  return String(s).replace(/[<>&"]/g,function(c){
    return {'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;'}[c];
  });
}
function bars(rssi){
  var n = rssi>=-55?4 : rssi>=-67?3 : rssi>=-78?2 : 1;
  var h='<div class="bars">';
  for(var i=0;i<4;i++){
    h+='<i class="'+(i<n?'on':'')+'" style="height:'+(4+i*3)+'px"></i>';
  }
  return h+'</div>';
}
function scan(){
  var p=$('apPanel'), b=$('scanBtn');
  b.textContent='扫描中';
  p.innerHTML='<div class="ap"><span class="nm">正在扫描附近网络…</span></div>';
  p.classList.add('open');
  fetch('/scan').then(function(r){return r.json()}).then(function(list){
    b.textContent='扫描';
    if(!list || !list.length){
      p.innerHTML='<div class="ap"><span class="nm">没扫到网络，靠近路由器再试</span></div>';
      return;
    }
    list.sort(function(a,b){return b.rssi-a.rssi});
    var html='';
    for(var i=0;i<list.length;i++){
      var ap=list[i];
      html+='<div class="ap" onclick="chooseAp(this)" data-ssid="'+esc(ap.ssid)+'">'+
            '<span class="nm">'+esc(ap.ssid)+'</span>'+bars(ap.rssi)+
            '<span class="sg">'+ap.rssi+'</span></div>';
    }
    p.innerHTML=html;
  }).catch(function(){
    b.textContent='扫描';
    p.innerHTML='<div class="ap"><span class="nm">扫描失败，请重试</span></div>';
  });
}
function chooseAp(el){
  $('ssid').value=el.getAttribute('data-ssid');
  $('apPanel').classList.remove('open');
  $('pwd').focus();
}
function clearPwd(){
  $('pwd').value='';
  $('pwd').focus();
}
function togglePwd(btn){
  var p=$('pwd');
  if(p.type==='password'){
    p.type='text';
    btn.classList.add('on');
  }else{
    p.type='password';
    btn.classList.remove('on');
  }
}

function load(){
  fetch('/state').then(function(r){return r.json()}).then(function(s){
    ST=s;
    ST.prov=!!s.prov;
    $('stDot').className='dot'+(s.connected?'':' off');
    $('stText').textContent=s.connected?('已连接'+(s.ssid?(' · '+s.ssid):'')):(s.provisioned?'未连接网络':'等待配网');
    if(s.connected){
      $('netTip').textContent='当前已连接 '+s.ssid+(s.rssi?('，信号 '+s.rssi+' dBm'):'');
      if(s.ssid){$('ssid').value=s.ssid;}   // 输入框显示"真实连接的网络"，不是历史记录
    }else{
      $('netTip').textContent='未连接。选择家里的 Wi-Fi 并输入密码。';
    }
    renderWifi();
    // 没连上网：自动进配网模式（暂停语音），让用户先配网
    if(!s.connected && !ST.prov){enterProv()}
    if(!s.provisioned){toast('请先连接 Wi-Fi')}
  }).catch(function(){});
  fetch('/user').then(function(r){return r.json()}).then(function(u){
    if(u.height)$('height').value=u.height;
    if(u.weight)$('weight').value=u.weight;
    if(u.age)$('age').value=u.age;
    if(u.gender)setIdx('gender',u.gender-1);
    if(u.ssid && !$('ssid').value){$('ssid').value=u.ssid}
    var p=u.prefs||{};
    setIdx('programs',p.program||0);
    setIdx('strength',(p.strength||3)-1);
    setIdx('speed',p.speed===undefined?1:p.speed);
    var dur=[10,20,30,45],di=0;
    for(var i=0;i<dur.length;i++){if(dur[i]===p.duration)di=i}
    setIdx('duration',di);
    setMask('parts',p.parts===undefined?7:p.parts);
    setIdx('airNeck',p.air_neck||0);
    setIdx('airShoulder',p.air_shoulder||0);
    setIdx('airArm',p.air_arm||0);
    setIdx('airLeg',p.air_leg||0);
    setIdx('airFoot',p.air_foot||0);
    setIdx('roller',p.foot_roller===undefined?2:p.foot_roller);
    setMask('heat',p.heat===undefined?3:p.heat);
    var ht=[40,45,50],hi=1;
    for(var k=0;k<ht.length;k++){if(ht[k]===p.heat_temp)hi=k}
    setIdx('heatTemp',hi);
    if(p.volume!==undefined){$('vol').value=p.volume;$('volv').textContent=p.volume+'%';$('vol').style.setProperty('--p',p.volume+'%')}
    setIdx('ttsSpeed',p.tts_speed||0);
    setIdx('ttsVoice',p.tts_voice||0);
    var fu=[0,15,60,600],fi=3;
    for(var m=0;m<fu.length;m++){if(fu[m]===p.follow_up)fi=m}
    setIdx('follow',fi);
    $('ack').checked=!!p.ack;
    $('lock').checked=!!p.child_lock;
    var sm=[0,30,60,120],si=0;
    for(var n=0;n<sm.length;n++){if(sm[n]===p.sedentary)si=n}
    setIdx('sed',si);
  }).catch(function(){});
}

// 两个入口彻底分开，避免"别的面板保存时顺带重连 Wi-Fi"：
//   saveWifi()  只由 Wi-Fi 卡里的「连 接」按钮调用，提交 Wi-Fi + 全部设置
//   savePrefs() 其他所有卡片的「保 存」按钮，只提交设置，绝不碰 Wi-Fi
function saveWifi(){
  var ssid=$('ssid').value.trim();
  var pwd=$('pwd').value;
  if(!ssid){toast('请先选择或输入 Wi-Fi 名称');return}
  if(!pwd){toast('请输入 Wi-Fi 密码');$('pwd').focus();return}
  savePrefs(true, ssid, pwd);
}
function savePrefs(withWifi, ssid, pwd){
  var body={};
  if(withWifi===true){
    body.ssid=ssid;
    body.password=pwd;
  }
  body.height=parseInt($('height').value,10)||0;
  body.weight=parseInt($('weight').value,10)||0;
  body.age=parseInt($('age').value,10)||0;
  body.gender=idx('gender')+1;
  var dur=[10,20,30,45],ht=[40,45,50],fu=[0,15,60,600],sm=[0,30,60,120];
  body.prefs={
    program:idx('programs'),
    strength:idx('strength')+1,
    speed:idx('speed'),
    duration:dur[idx('duration')],
    parts:mask('parts'),
    air_neck:idx('airNeck'),
    air_shoulder:idx('airShoulder'),
    air_arm:idx('airArm'),
    air_leg:idx('airLeg'),
    air_foot:idx('airFoot'),
    foot_roller:idx('roller'),
    heat:mask('heat'),
    heat_temp:ht[idx('heatTemp')],
    volume:parseInt($('vol').value,10),
    tts_speed:idx('ttsSpeed'),
    tts_voice:idx('ttsVoice'),
    follow_up:fu[idx('follow')],
    ack:$('ack').checked?1:0,
    child_lock:$('lock').checked?1:0,
    sedentary:sm[idx('sed')]
  };
  toast(withWifi===true ? ('正在连接 '+ssid+'…') : '保存中…');
  postSave(body,0);
}

function postSave(body,retry){
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(function(r){return r.json()})
    .then(function(j){
      if(!j.ok){toast(j.error||'保存失败');return}
      if(body.ssid){
        // 保存 Wi-Fi 时设备会切 AP 信道，手机会短暂掉线，所以轮询状态等结果
        toast('已保存，正在连接 '+body.ssid+'…');
        pollState(body.ssid,0);
      }else{
        toast('已保存');
        setTimeout(load,600);
      }
    })
    .catch(function(){
      if(retry<1){
        // 切信道瞬间会连不上，隔 2 秒重试一次
        setTimeout(function(){postSave(body,retry+1)},2000);
      }else{
        toast('保存失败，请检查连接');
      }
    });
}

function pollState(ssid,n){
  if(n>15){toast('连接超时，请检查密码后重试');return}
  setTimeout(function(){
    fetch('/state').then(function(r){return r.json()}).then(function(s){
      ST=s;
      if(s.connected && (!ssid || s.ssid===ssid)){
        ST=s; ST.prov=false;
        toast('Wi-Fi 已连接：'+s.ssid);
        $('pwd').value='';
        $('stDot').className='dot';
        $('stText').textContent='已连接 · '+s.ssid;
        renderWifi();
        return;
      }
      pollState(ssid,n+1);
    }).catch(function(){pollState(ssid,n+1)});
  },1500);
}

var holdT=null,holdMs=0;
function holdStart(){
  if(holdT) return;              // mousedown 和 touchstart 会同时触发，防止起两个计时器
  holdMs=0;
  $('resetBtn').classList.add('holding');
  holdT=setInterval(function(){
    holdMs+=100;
    $('resetBtn').textContent='继续按住… '+((3000-holdMs)/1000).toFixed(1)+'s';
    if(holdMs>=3000){holdEnd();doReset()}
  },100);
}
function holdEnd(){
  if(holdT){clearInterval(holdT);holdT=null}
  var b=$('resetBtn');
  if(!b) return;
  b.classList.remove('holding');
  b.textContent='长按 3 秒清除所有设置';
}
// 用户切走页面/切后台/松手丢失事件时，必须停掉倒计时，否则会"回来就发现被恢复了出厂"
document.addEventListener('visibilitychange',function(){if(document.hidden)holdEnd()});
window.addEventListener('pagehide',holdEnd);
window.addEventListener('blur',holdEnd);
function doReset(){
  toast('正在恢复出厂…');
  fetch('/reset',{method:'POST'})
    .then(function(){toast('已重置，设备重启中')})
    .catch(function(){toast('已提交，设备重启中')});
}

load();
</script>
</body>
</html>)HTML";
