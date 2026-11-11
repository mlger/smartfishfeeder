/**
 * Catfish Feeder — LVGL-style XML UI renderer for the GitHub Pages demo.
 *
 * Parses web-flasher/demo/ui/catfish-ui.xml and maps the widget tree onto
 * DOM at the panel's true 800x480. Property names follow the firmware's
 * LVGL vocabulary (see docs/figma-lvgl.md and the header comment in the
 * XML file). This is a preview interpreter, not an LVGL port — the device
 * itself runs hand-written LVGL v8 C code in firmware/display-unit/src/ui/.
 *
 * Layout model:
 *   - A widget without `align` participates in its parent's flex flow
 *     (or stacks statically if the parent has no flex).
 *   - A widget with `align` is absolutely positioned inside its parent
 *     using the LV_ALIGN anchor + ox/oy offsets (pure CSS custom props).
 */
(function () {
  'use strict';

  var XML_URL = 'ui/catfish-ui.xml';
  var HOLD_MS = 800; // firmware FEED_HOLD_MS
  var BEZEL = 18; // px of device frame around the 800x480 stage

  // ---------------------------------------------------------- runtime state
  var state = {
    screen: 'home',
    clock12: true,
    vacation: false,
    backlight: true,
    tankCount: 1,
    fedToday: 3,
    lastValue: 'Yesterday',
    lastSub: '18:00 • 25 g',
    portion: 15,
    holdRAF: null,
    holdStart: 0,
    holdDone: false,
    scanning: false,
    xmlText: '',
  };
  var byId = {}; // id -> element (per current DOM build)
  var binds = {}; // bind name -> [elements]

  // ---------------------------------------------------------------- helpers
  function px(v) {
    // XML attrs arrive as strings: '420' -> '420px', '100%' stays '100%'
    return /^-?\d+(\.\d+)?$/.test(v) ? v + 'px' : v;
  }

  function num(el, name, dflt) {
    var v = el.getAttribute(name);
    if (v === null || v === '') return dflt;
    var n = parseFloat(v);
    return isNaN(n) ? dflt : n;
  }

  function str(el, name, dflt) {
    var v = el.getAttribute(name);
    return v === null ? dflt : v;
  }

  function bool(el, name, dflt) {
    var v = el.getAttribute(name);
    if (v === null) return dflt;
    return v === 'true' || v === '1';
  }

  // LV_ALIGN kebab-case -> CSS anchors. Offsets ride in --ox/--oy so the
  // static stylesheet can compose them with 50% anchors via calc().
  var ALIGN_CSS = {
    'top-left': { left: 'calc(var(--ox, 0px))', top: 'calc(var(--oy, 0px))' },
    'top-mid': { left: 'calc(50% + var(--ox, 0px))', top: 'calc(var(--oy, 0px))', tf: 'translateX(-50%)' },
    'top-right': { right: 'calc(0px - var(--ox, 0px))', top: 'calc(var(--oy, 0px))' },
    'left-mid': { left: 'calc(var(--ox, 0px))', top: 'calc(50% + var(--oy, 0px))', tf: 'translateY(-50%)' },
    'center': { left: 'calc(50% + var(--ox, 0px))', top: 'calc(50% + var(--oy, 0px))', tf: 'translate(-50%, -50%)' },
    'right-mid': { right: 'calc(0px - var(--ox, 0px))', top: 'calc(50% + var(--oy, 0px))', tf: 'translateY(-50%)' },
    'bottom-left': { left: 'calc(var(--ox, 0px))', bottom: 'calc(0px - var(--oy, 0px))' },
    'bottom-mid': { left: 'calc(50% + var(--ox, 0px))', bottom: 'calc(0px - var(--oy, 0px))', tf: 'translateX(-50%)' },
    'bottom-right': { right: 'calc(0px - var(--ox, 0px))', bottom: 'calc(0px - var(--oy, 0px))' },
  };

  var FLEX_MAIN = {
    center: 'center',
    start: 'flex-start',
    end: 'flex-end',
    'space-between': 'space-between',
    'space-evenly': 'space-evenly',
  };
  var FLEX_CROSS = { center: 'center', start: 'flex-start', end: 'flex-end' };

  function colorOf(el, name, consts, dflt) {
    var v = el.getAttribute(name);
    if (v === null) return dflt;
    v = v.trim();
    if (v.charAt(0) === '$') return consts[v.slice(1)] || dflt;
    return v;
  }

  // ------------------------------------------------------------- widget tree
  function buildWidget(el, consts, parent) {
    var tag = el.tagName.toLowerCase();
    if (tag === 'consts') return null;

    var kind = tag === 'screen' ? 'screen' : tag;
    var d = document.createElement('div');
    d.className = 'lv-' + kind;

    var id = str(el, 'id', null);
    if (id) { d.id = 'ui-' + id; byId[id] = d; }

    var bind = str(el, 'bind', null);
    if (bind) {
      d.setAttribute('data-bind', bind);
      (binds[bind] = binds[bind] || []).push(d);
    }

    applyStyles(d, el, consts);

    if (bool(el, 'hidden', false)) d.classList.add('lv-hidden');
    if (bool(el, 'disabled', false)) d.classList.add('lv-disabled');
    if (bool(el, 'scroll', false)) d.classList.add('lv-scroll');

    // text content — labels/buttons/s carry their text in the `text`
    // attribute (element children are separate widgets)
    var text = el.getAttribute('text');
    if (text) d.textContent = text;

    // children
    var kids = [];
    for (var i = 0; i < el.children.length; i++) kids.push(el.children[i]);
    kids.forEach(function (k) {
      var c = buildWidget(k, consts, d);
      if (c) d.appendChild(c);
    });

    wireEvents(d, el);
    return d;
  }

  function applyStyles(d, el, consts) {
    var s = d.style;
    var tag = el.tagName.toLowerCase();

    // LVGL semantics: widgets with a set size never shrink inside flex
    // parents (the default flex-shrink:1 collapsed fixed-height children)
    s.flexShrink = '0';

    // size
    var w = str(el, 'w', null);
    var h = str(el, 'h', null);
    if (w) s.width = px(w);
    if (h) s.height = px(h);

    // alignment: absolute positioning inside the parent (inline position so
    // it always wins over the .lv-panel relative rule)
    var align = str(el, 'align', null);
    if (align && ALIGN_CSS[align]) {
      d.classList.add('lv-abs');
      s.position = 'absolute';
      var css = ALIGN_CSS[align];
      s.left = css.left || 'auto';
      s.top = css.top || 'auto';
      s.right = css.right || 'auto';
      s.bottom = css.bottom || 'auto';
      s.setProperty('--ox', num(el, 'ox', 0) + 'px');
      s.setProperty('--oy', num(el, 'oy', 0) + 'px');
      if (css.tf) s.transform = css.tf;
      // ox/oy also serve as plain x/y when the anchor already fixes the axis
      if (str(el, 'x', null) !== null) s.setProperty('--ox', num(el, 'x', 0) + 'px');
      if (str(el, 'y', null) !== null) s.setProperty('--oy', num(el, 'y', 0) + 'px');
    } else {
      // free x/y inside non-flex parents
      if (str(el, 'x', null) !== null || str(el, 'y', null) !== null) {
        d.classList.add('lv-abs');
        s.position = 'absolute';
        s.left = num(el, 'x', 0) + 'px';
        s.top = num(el, 'y', 0) + 'px';
      }
    }

    // colors — bg rides in --bg so the :active rule can swap to --bg-pressed
    // (an inline background would beat the stylesheet's :active)
    var bg = colorOf(el, 'bg-color', consts, null);
    if (bg) s.setProperty('--bg', bg);
    var bgp = colorOf(el, 'bg-pressed', consts, null);
    if (bgp) s.setProperty('--bg-pressed', bgp);
    var fg = colorOf(el, 'color', consts, null);
    if (fg) s.color = fg;
    var bc = colorOf(el, 'border-color', consts, null);
    if (bc) {
      s.borderColor = bc;
      s.borderWidth = num(el, 'border-width', 1) + 'px';
      s.borderStyle = 'solid';
    }

    // shape + spacing
    var r = num(el, 'radius', -1);
    if (r >= 0) s.borderRadius = r + 'px';
    var pad = num(el, 'pad', -1);
    if (pad >= 0) s.padding = pad + 'px';

    // flex layout
    var flex = str(el, 'flex', null);
    if (flex) {
      s.display = 'flex';
      s.flexDirection = flex === 'row' ? 'row' : 'column';
      var pr = num(el, 'pad-row', 0);
      var pc = num(el, 'pad-col', 0);
      s.gap = (flex === 'row' ? pc : pr) + 'px';
      var fm = str(el, 'flex-main', null);
      if (fm && FLEX_MAIN[fm]) s.justifyContent = FLEX_MAIN[fm];
      var fc = str(el, 'flex-cross', null);
      if (fc && FLEX_CROSS[fc]) s.alignItems = FLEX_CROSS[fc];
    } else {
      // a plain container that still wants centered children
      var fm2 = str(el, 'flex-main', null);
      if (fm2 === 'center') {
        s.display = 'flex';
        s.justifyContent = 'center';
        s.alignItems = 'center';
      }
    }
    var grow = num(el, 'grow', 0);
    if (grow > 0) s.flexGrow = grow;

    // text
    if (tag === 'label' || tag === 'button') {
      var f = num(el, 'font', tag === 'button' ? 16 : 14);
      s.fontSize = f + 'px';
      var ls = num(el, 'letter-space', 0);
      if (ls) s.letterSpacing = ls + 'px';
      d.classList.add(f >= 20 ? 'lv-bold' : 'lv-medium');
      s.whiteSpace = 'nowrap';
    }
  }

  // ---------------------------------------------------------------- events
  function wireEvents(d, el) {
    var tag = el.tagName.toLowerCase();
    if (tag !== 'button' && tag !== 'switch') return;

    if (tag === 'switch') {
      d.classList.toggle('on', bool(el, 'on', false));
      d.setAttribute('data-key', str(el, 'key', ''));
      d.setAttribute('role', 'switch');
      d.setAttribute('aria-checked', bool(el, 'on', false) ? 'true' : 'false');
      d.setAttribute('tabindex', '0');
      d.addEventListener('click', function () { toggleSwitch(d); });
      d.addEventListener('keydown', function (e) {
        if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggleSwitch(d); }
      });
      return;
    }

    var nav = str(el, 'nav', null);
    if (nav !== null) {
      d.addEventListener('click', function () { showScreen(parseInt(nav, 10)); });
      return;
    }
    if (str(el, 'hold', null) === 'feed') { wireHoldToFeed(d); return; }
    var action = str(el, 'action', null);
    if (action) d.addEventListener('click', function () { runAction(action, d); });
  }

  function toggleSwitch(d) {
    var on = !d.classList.contains('on');
    d.classList.toggle('on', on);
    d.setAttribute('aria-checked', on ? 'true' : 'false');
    var key = d.getAttribute('data-key');
    if (key === 'clock12') { state.clock12 = on; tick(); }
    else if (key === 'vacation') { state.vacation = on; applyVacation(); }
    else if (key === 'backlight') { state.backlight = on; toast(on ? 'Backlight on' : 'Backlight off'); }
    else if (key && key.indexOf('sched') === 0) {
      toast(on ? 'Schedule enabled' : 'Schedule paused');
    }
  }

  function applyVacation() {
    var badge = byId['badge-vacation'];
    if (badge) badge.classList.toggle('lv-hidden', !state.vacation);
    var feed = byId['feed-btn'];
    if (feed) feed.classList.toggle('lv-disabled', state.vacation);
    toast(state.vacation ? 'Vacation — schedules paused' : 'Vacation off — schedules resume');
  }

  // ------------------------------------------------- hold-to-confirm FEED NOW
  // Mirrors the firmware: tap < 800 ms never feeds ("Press and hold"), a full
  // 800 ms hold runs the amber fill behind the label and then feeds.
  function wireHoldToFeed(btn) {
    var fill = document.createElement('div');
    fill.className = 'lv-holdfill';
    btn.insertBefore(fill, btn.firstChild);

    function start(e) {
      if (state.vacation) { toast('Vacation — schedules paused'); return; }
      if (btn.classList.contains('lv-disabled')) return;
      e.preventDefault();
      state.holdStart = performance.now();
      state.holdDone = false;
      fill.style.transition = 'none';
      fill.style.width = '0%';
      var t0 = state.holdStart;
      function step(t) {
        var p = Math.min(1, (t - t0) / HOLD_MS);
        fill.style.width = (p * 100).toFixed(1) + '%';
        if (p < 1) state.holdRAF = requestAnimationFrame(step);
        else complete();
      }
      state.holdRAF = requestAnimationFrame(step);
    }
    function complete() {
      state.holdDone = true;
      reset();
      doFeed();
    }
    function cancel() {
      if (state.holdDone) return;
      var held = performance.now() - state.holdStart;
      reset();
      if (state.holdStart && held < HOLD_MS) toast('Press and hold to feed');
      state.holdStart = 0;
    }
    function reset() {
      if (state.holdRAF) cancelAnimationFrame(state.holdRAF);
      state.holdRAF = null;
      fill.style.transition = 'width .25s ease';
      fill.style.width = '0%';
    }

    btn.addEventListener('pointerdown', start);
    btn.addEventListener('pointerup', cancel);
    btn.addEventListener('pointerleave', cancel);
    btn.addEventListener('pointercancel', cancel);
  }

  function doFeed() {
    state.fedToday += 1;
    var now = new Date();
    state.lastValue = 'Just now';
    state.lastSub = fmtTime(now, false) + ' • ' + state.portion + ' g';
    updateBind('last-value', state.lastValue);
    updateBind('last-sub', state.lastSub);
    updateBind('feed-preview',
      state.portion + ' g • All tanks • ' + state.fedToday + ' fed today');
    toast('Fed ' + state.portion + ' g • All tanks');
  }

  // ------------------------------------------------------------- actions
  function runAction(name, btn) {
    switch (name) {
      case 'scan':
        if (state.scanning) return;
        state.scanning = true;
        btn.classList.add('lv-disabled');
        toast('Scanning the WiFi LAN…');
        setTimeout(function () {
          state.scanning = false;
          btn.classList.remove('lv-disabled');
          toast('Scan: 2 relay(s) live');
        }, 1200);
        break;
      case 'add-time': toast('NEW FEEDING TIME dialog opens here (demo)'); break;
      case 'edit': toast('Schedule editor opens here (demo)'); break;
      case 'token': toast('Pairing token keyboard opens here (demo)'); break;
      case 'tank-inc':
        state.tankCount = Math.min(4, state.tankCount + 1);
        updateBind('tank-count', String(state.tankCount));
        break;
      case 'tank-dec':
        state.tankCount = Math.max(1, state.tankCount - 1);
        updateBind('tank-count', String(state.tankCount));
        break;
      default: toast('Demo action: ' + name);
    }
  }

  function updateBind(name, text) {
    (binds[name] || []).forEach(function (el) { el.textContent = text; });
  }

  // ------------------------------------------------------------- screens
  function showScreen(idx) {
    var order = ['home', 'schedule', 'settings'];
    state.screen = order[idx] || 'home';
    var screens = document.querySelectorAll('#stage .lv-screen');
    for (var i = 0; i < screens.length; i++) {
      screens[i].classList.toggle('lv-screen-active', i === idx);
    }
    renderXmlSource(order[idx]);
    var pills = document.querySelectorAll('.screen-pill');
    for (var j = 0; j < pills.length; j++) {
      pills[j].classList.toggle('pill-active', j === idx);
    }
  }

  // ---------------------------------------------------------------- clock
  function fmtTime(d, secs) {
    var h = d.getHours();
    var m = d.getMinutes();
    var s = d.getSeconds();
    var min = (m < 10 ? '0' : '') + m;
    var sec = (s < 10 ? '0' : '') + s;
    if (state.clock12) {
      var ap = h >= 12 ? 'PM' : 'AM';
      var h12 = h % 12; if (h12 === 0) h12 = 12;
      return (h12 < 10 ? ' ' : '') + h12 + ':' + min + (secs ? ':' + sec : '') + ' ' + ap;
    }
    return (h < 10 ? '0' : '') + h + ':' + min + (secs ? ':' + sec : '');
  }

  function tick() {
    updateBind('clock', fmtTime(new Date(), false));
  }

  // ---------------------------------------------------------------- toast
  var toastTimer = null;
  function toast(msg) {
    var t = document.getElementById('toast');
    if (!t) return;
    t.textContent = msg;
    t.classList.add('show');
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.classList.remove('show'); }, 2600);
  }

  // --------------------------------------------------------- XML source tab
  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function highlight(src) {
    // single pass so injected spans are never re-tokenized.
    // NOTE the callback unpacks ALL FOUR capture groups: com, tag, attr name,
    // attr value — missing one silently shifted the args (the v1 bug that
    // rendered `te=t` instead of `text="..."`).
    return esc(src).replace(
      /(&lt;!--[\s\S]*?--&gt;)|(&lt;\/?[a-zA-Z-]+|\/?&gt;)|([a-zA-Z-]+)=("[^"]*")/g,
      function (m, com, tag, name, val) {
        if (com) return '<span class="c-com">' + com + '</span>';
        if (tag) return '<span class="c-tag">' + tag + '</span>';
        if (name) {
          return '<span class="c-attr">' + name + '</span>=' +
                 '<span class="c-str">' + val + '</span>';
        }
        return m;
      }
    );
  }

  function renderXmlSource(screenId) {
    var pre = document.getElementById('xml-source');
    if (!pre || !state.xmlText) return;
    var re = new RegExp('[ \\t]*<screen id="' + screenId + '"[\\s\\S]*?<\\/screen>');
    var m = state.xmlText.match(re);
    pre.innerHTML = highlight(m ? m[0] : state.xmlText);
    var lbl = document.getElementById('xml-screen-label');
    if (lbl) lbl.textContent = 'screen id="' + screenId + '"';
  }

  // ---------------------------------------------------------------- scaling
  function fitStage() {
    var wrap = document.getElementById('stage-wrap');
    var sc = document.getElementById('stage-scale');
    if (!wrap || !sc) return;
    var avail = wrap.clientWidth - 2 * BEZEL - 8;
    var s = Math.min(1, Math.max(0.25, avail / 800));
    // scale from the top-left and center explicitly — a 'top center' origin
    // on an overflowing 800px box pushes the visual off-center on phones
    sc.style.transformOrigin = 'top left';
    var offX = Math.max(0, (wrap.clientWidth - 800 * s) / 2);
    sc.style.transform = 'translateX(' + offX.toFixed(1) + 'px) scale(' + s + ')';
    wrap.style.height = (480 + 2 * BEZEL) * s + 8 + 'px';
  }

  // ------------------------------------------------------------------ boot
  function boot(xmlDoc) {
    var root = xmlDoc.documentElement;
    var consts = {};
    var constEls = root.getElementsByTagName('color');
    for (var i = 0; i < constEls.length; i++) {
      consts[constEls[i].getAttribute('name')] = constEls[i].getAttribute('value');
    }

    var stage = document.getElementById('stage');
    var screens = root.getElementsByTagName('screen');
    for (var k = 0; k < screens.length; k++) {
      var node = buildWidget(screens[k], consts, stage);
      if (node) {
        node.classList.add('lv-screen');
        stage.appendChild(node);
      }
    }
    showScreen(0);
    tick();
    setInterval(tick, 1000);
    window.addEventListener('resize', fitStage);
    fitStage();
  }

  // page-level screen pills + console hook
  window.__catfishDemo = { showScreen: showScreen, state: state };

  document.addEventListener('DOMContentLoaded', function () {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', XML_URL, true);
    xhr.onload = function () {
      if (xhr.status !== 200) {
        document.getElementById('stage').innerHTML =
          '<p class="load-err">Could not load ' + XML_URL + ' (HTTP ' +
          xhr.status + ') — serve the demo over HTTP(S).</p>';
        return;
      }
      state.xmlText = xhr.responseText;
      var doc = new DOMParser().parseFromString(xhr.responseText, 'application/xml');
      var err = doc.getElementsByTagName('parsererror');
      if (err.length) {
        document.getElementById('stage').innerHTML =
          '<p class="load-err">catfish-ui.xml failed to parse.</p>';
        return;
      }
      boot(doc);
      renderXmlSource('home');
    };
    xhr.onerror = function () {
      document.getElementById('stage').innerHTML =
        '<p class="load-err">Could not fetch ' + XML_URL + ' — open this demo from the Pages site (fetch needs HTTP/S).</p>';
    };
    xhr.send();

    var cp = document.getElementById('xml-copy');
    if (cp) {
      cp.addEventListener('click', function () {
        var re = new RegExp('[ \\t]*<screen id="' + state.screen + '"[\\s\\S]*?<\\/screen>');
        var m = state.xmlText.match(re);
        var txt = m ? m[0] : state.xmlText;
        if (navigator.clipboard) {
          navigator.clipboard.writeText(txt).then(function () {
            cp.textContent = 'Copied!';
            setTimeout(function () { cp.textContent = 'Copy XML'; }, 1500);
          });
        }
      });
    }
  });
})();
