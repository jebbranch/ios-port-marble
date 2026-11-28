/* 语言切换：默认简体中文，从不自动跳转。
   只做两件事——记住用户点过的语言；在首次访问且浏览器语言与本页不符时，
   在底部提一句，提过一次就不再提。 */
(function () {
  var CHOSEN = 'marble-lang';
  var HUSHED = 'marble-lang-hushed';
  var here = document.documentElement.lang || 'zh-CN';

  function pick() {
    var list = navigator.languages || [navigator.language || 'en'];
    for (var i = 0; i < list.length; i++) {
      var n = String(list[i]).toLowerCase();
      if (/^zh/.test(n)) return /-(tw|hk|mo)$|hant/.test(n) ? 'zh-Hant' : 'zh-CN';
      if (/^(en|de|es|fr|pt|ru|it|nl|pl|tr|id|vi|ar|th)/.test(n)) return 'en';
    }
    return 'en';
  }

  var want;
  try {
    want = localStorage.getItem(CHOSEN) || (!localStorage.getItem(HUSHED) && pick());
  } catch (e) {
    want = pick();
  }

  document.querySelectorAll('.lang a').forEach(function (a) {
    a.addEventListener('click', function () {
      try { localStorage.setItem(CHOSEN, a.dataset.lang); } catch (e) {}
    });
  });

  if (!want || want === here) return;

  var link = document.querySelector('.lang a[data-lang="' + want + '"]');
  if (!link) return;

  var SAY = {
    'zh-CN': { 'zh-Hant': '本页另有繁體中文版本。', en: '本页另有 English 版本。' },
    'zh-Hant': { 'zh-CN': '此頁另有简体中文版本。', en: '此頁另有 English 版本。' },
    en: { 'zh-CN': 'This page is also available in 简体中文。', 'zh-Hant': 'This page is also available in 繁體中文。' }
  };

  var text = (SAY[here] || {})[want];
  if (!text) return;

  var bar = document.createElement('div');
  bar.className = 'langbar';
  bar.setAttribute('role', 'status');

  var go = document.createElement('a');
  go.href = link.href;
  go.textContent = text;

  var hush = document.createElement('button');
  hush.type = 'button';
  hush.className = 'langbar-x';
  hush.textContent = '×';
  hush.setAttribute('aria-label', 'close');

  bar.appendChild(go);
  bar.appendChild(hush);
  document.body.appendChild(bar);

  hush.addEventListener('click', function () {
    try { localStorage.setItem(HUSHED, '1'); } catch (e) {}
    bar.remove();
  });
})();
