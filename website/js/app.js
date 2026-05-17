(function () {
  'use strict';

  const STORAGE_THEME = 'topitene-theme';
  const STORAGE_TAB = 'topitene-tab';

  const THEMES = ['light', 'dark', 'iu5'];
  const LOGO_BY_THEME = {
    light: 'assets/brand_light.png',
    dark: 'assets/brand_dark.png',
    iu5: 'assets/brand_iu5.png',
  };

  const TAB_META = {
    about: {
      title: 'О проекте',
      subtitle: 'Информация о СУБД topiteneninglmsabce_и_гаяна',
    },
    team: {
      title: 'Команда',
      subtitle: 'Разработчики проекта',
    },
    install: {
      title: 'Установка',
      subtitle: 'Скачивание клиента для Windows и Linux',
    },
    mistral: {
      title: 'Mistral API',
      subtitle: 'Регистрация и получение API-ключа',
    },
    gol: {
      title: 'ГОООООЛ',
      subtitle: '',
    },
  };

  const body = document.body;
  const brandLogo = document.getElementById('brand-logo');
  const sectionTitle = document.getElementById('section-title');
  const sectionSubtitle = document.getElementById('section-subtitle');
  const navItems = document.querySelectorAll('.drawer-nav-item[data-tab]');
  const tabPanels = document.querySelectorAll('.tab-panel');
  const themeButtons = document.querySelectorAll('.theme-btn[data-theme]');
  const menuTrigger = document.getElementById('menu-trigger');
  const drawerClose = document.getElementById('drawer-close');
  const navBackdrop = document.getElementById('nav-backdrop');
  const navDrawer = document.getElementById('nav-drawer');

  const downloadWindows = document.getElementById('download-windows');
  const downloadLinux = document.getElementById('download-linux');
  const goalVideo = document.getElementById('goal-video');
  const pageHeader = document.querySelector('.page-header');

  let currentTab = 'about';
  let tabSwitchTimer = null;

  function applyTheme(theme) {
    if (!THEMES.includes(theme)) theme = 'light';
    body.classList.remove('theme-light', 'theme-dark', 'theme-iu5');
    body.classList.add('theme-' + theme);

    themeButtons.forEach((btn) => {
      btn.classList.toggle('active', btn.dataset.theme === theme);
    });

    if (brandLogo && LOGO_BY_THEME[theme]) {
      brandLogo.src = LOGO_BY_THEME[theme];
    }

    try {
      localStorage.setItem(STORAGE_THEME, theme);
    } catch (_) { /* ignore */ }
  }

  function setDrawerOpen(open) {
    if (!menuTrigger || !navDrawer || !navBackdrop) return;

    menuTrigger.setAttribute('aria-expanded', open ? 'true' : 'false');
    navDrawer.setAttribute('aria-hidden', open ? 'false' : 'true');
    navBackdrop.setAttribute('aria-hidden', open ? 'false' : 'true');

    navDrawer.classList.toggle('is-open', open);
    navBackdrop.classList.toggle('is-visible', open);
    body.classList.toggle('drawer-open', open);

    if (open) {
      navBackdrop.removeAttribute('hidden');
      requestAnimationFrame(() => {
        navBackdrop.classList.add('is-visible');
      });
    } else {
      navBackdrop.classList.remove('is-visible');
      const onEnd = (e) => {
        if (e.target !== navBackdrop || e.propertyName !== 'opacity') return;
        navBackdrop.setAttribute('hidden', '');
        navBackdrop.removeEventListener('transitionend', onEnd);
      };
      navBackdrop.addEventListener('transitionend', onEnd);
    }
  }

  function updateNavActive(tabId) {
    navItems.forEach((btn) => {
      const active = btn.dataset.tab === tabId;
      btn.classList.toggle('active', active);
      btn.setAttribute('aria-current', active ? 'page' : null);
    });
  }

  function updateHeader(tabId) {
    const meta = TAB_META[tabId];
    if (!meta) return;
    const isGoal = tabId === 'gol';
    body.classList.toggle('is-goal-tab', isGoal);
    if (pageHeader) pageHeader.hidden = isGoal;
    if (sectionTitle) sectionTitle.textContent = meta.title;
    if (sectionSubtitle) {
      sectionSubtitle.textContent = meta.subtitle;
      sectionSubtitle.hidden = isGoal || !meta.subtitle;
    }
  }

  function handleGoalVideo(tabId) {
    if (!goalVideo) return;
    if (tabId === 'gol') {
      goalVideo.currentTime = 0;
    } else {
      goalVideo.pause();
    }
  }

  function showTabPanel(tabId) {
    const nextPanel = document.getElementById('tab-' + tabId);
    if (!nextPanel) return;

    const prevPanel = document.querySelector('.tab-panel.active');
    if (prevPanel === nextPanel) return;

    if (tabSwitchTimer) {
      clearTimeout(tabSwitchTimer);
      tabSwitchTimer = null;
    }

    if (prevPanel) {
      prevPanel.classList.add('is-leaving');
      prevPanel.classList.remove('active');

      tabSwitchTimer = window.setTimeout(() => {
        prevPanel.classList.remove('is-leaving');
        prevPanel.hidden = true;

        nextPanel.hidden = false;
        requestAnimationFrame(() => {
          nextPanel.classList.add('active');
        });
        tabSwitchTimer = null;
      }, 220);
    } else {
      tabPanels.forEach((p) => {
        p.hidden = true;
        p.classList.remove('active', 'is-leaving');
      });
      nextPanel.hidden = false;
      nextPanel.classList.add('active');
    }
  }

  function applyTab(tabId, options = {}) {
    if (!TAB_META[tabId]) tabId = 'about';
    currentTab = tabId;

    updateNavActive(tabId);
    updateHeader(tabId);
    handleGoalVideo(tabId);
    showTabPanel(tabId);

    if (options.closeDrawer !== false && navDrawer?.classList.contains('is-open')) {
      setDrawerOpen(false);
    }

    try {
      localStorage.setItem(STORAGE_TAB, tabId);
    } catch (_) { /* ignore */ }

    if (options.updateHash !== false) {
      history.replaceState(null, '', '#' + tabId);
    }
  }

  function initTheme() {
    let theme = 'light';
    try {
      const saved = localStorage.getItem(STORAGE_THEME);
      if (saved && THEMES.includes(saved)) theme = saved;
    } catch (_) { /* ignore */ }
    applyTheme(theme);
  }

  function initTab() {
    let tab = 'about';
    try {
      const saved = localStorage.getItem(STORAGE_TAB);
      if (saved && TAB_META[saved]) tab = saved;
    } catch (_) { /* ignore */ }
    const hash = location.hash.replace(/^#/, '');
    if (hash && TAB_META[hash]) tab = hash;

    tabPanels.forEach((p) => {
      const isActive = p.id === 'tab-' + tab;
      p.hidden = !isActive;
      p.classList.toggle('active', isActive);
    });
    currentTab = tab;
    updateNavActive(tab);
    updateHeader(tab);
    handleGoalVideo(tab);
  }

  function enableDownloadLink(anchor) {
    if (!anchor) return;
    anchor.classList.remove('disabled');
    anchor.removeAttribute('aria-disabled');
    const badge = anchor.querySelector('.badge-soon');
    if (badge) badge.remove();
  }

  async function checkReleaseAvailable(anchor) {
    if (!anchor || !anchor.href) return;
    const url = new URL(anchor.getAttribute('href'), location.href).href;
    try {
      const res = await fetch(url, { method: 'HEAD' });
      if (res.ok) enableDownloadLink(anchor);
    } catch (_) { /* file:// or missing */ }
  }

  function initGuidePngFallback() {
    document.querySelectorAll('.guide-screenshot img[data-fallback-png]').forEach((img) => {
      const pngName = img.dataset.fallbackPng;
      const base = img.src.replace(/[^/]+$/, '');
      const pngSrc = base + pngName;
      const probe = new Image();
      probe.onload = () => {
        img.src = pngSrc;
        img.closest('.guide-screenshot')?.classList.remove('placeholder');
      };
      probe.src = pngSrc;
    });
  }

  themeButtons.forEach((btn) => {
    btn.addEventListener('click', () => applyTheme(btn.dataset.theme));
  });

  navItems.forEach((btn) => {
    btn.addEventListener('click', () => {
      applyTab(btn.dataset.tab, { closeDrawer: true });
    });
  });

  menuTrigger?.addEventListener('click', () => {
    const isOpen = menuTrigger.getAttribute('aria-expanded') === 'true';
    setDrawerOpen(!isOpen);
  });

  drawerClose?.addEventListener('click', () => setDrawerOpen(false));

  navBackdrop?.addEventListener('click', () => setDrawerOpen(false));

  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') setDrawerOpen(false);
  });

  window.addEventListener('hashchange', () => {
    const hash = location.hash.replace(/^#/, '');
    if (hash && TAB_META[hash]) applyTab(hash, { updateHash: false });
  });

  initTheme();
  initTab();
  initGuidePngFallback();

  if (location.protocol.startsWith('http')) {
    checkReleaseAvailable(downloadWindows);
    checkReleaseAvailable(downloadLinux);
  }
})();
