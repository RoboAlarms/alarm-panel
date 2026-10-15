/* Before the first paint: the theme the person chose (the toggle), else this computer's own
 * light or dark setting (FLS-017). A blocked storage API must not stop the page. */
try {
  const saved = localStorage.getItem('roboalarms-flasher-theme');
  document.documentElement.dataset.theme = saved === 'light' || saved === 'dark' ? saved
    : (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
} catch (e) {
  document.documentElement.dataset.theme = 'light';
}
