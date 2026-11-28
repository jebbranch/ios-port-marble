/* 时间轴：条目进入视口时点亮；竖线随滚动填充。
   没有 IntersectionObserver 或用户要求减少动效时，全部直接显示。 */
(function () {
  var wrap = document.querySelector('.tl-wrap');
  if (!wrap) return;

  var items = Array.prototype.slice.call(wrap.querySelectorAll('.tl-item'));
  var fill = wrap.querySelector('.tl-rail-fill');
  var reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  if (!reduce && 'IntersectionObserver' in window) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (!e.isIntersecting) return;
        e.target.classList.add('in');
        io.unobserve(e.target);
      });
    }, { rootMargin: '0px 0px -10% 0px', threshold: .08 });
    items.forEach(function (i) { io.observe(i); });
  } else {
    items.forEach(function (i) { i.classList.add('in'); });
  }

  if (!fill) return;

  var queued = 0;

  function measure() {
    queued = 0;
    var r = wrap.getBoundingClientRect();
    if (!r.height) return;
    var anchor = window.innerHeight * .58;
    var p = (anchor - r.top) / r.height;
    p = p < 0 ? 0 : p > 1 ? 1 : p;
    fill.style.height = (p * r.height).toFixed(1) + 'px';
  }

  function onScroll() {
    if (!queued) queued = requestAnimationFrame(measure);
  }

  addEventListener('scroll', onScroll, { passive: true });
  addEventListener('resize', onScroll);
  measure();
})();
