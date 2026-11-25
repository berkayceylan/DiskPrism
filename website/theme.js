// Runs before first paint: applies the stored or system theme and keeps the toggle in step.
(() => {
  const root = document.documentElement;
  const system = matchMedia("(prefers-color-scheme: dark)");
  const stored = () => {
    try {
      return localStorage.getItem("diskprism-theme");
    } catch {
      return null;
    }
  };
  const apply = (theme) => {
    root.dataset.theme = theme;
    document.dispatchEvent(new CustomEvent("themechange", { detail: theme }));
  };
  apply(stored() ?? (system.matches ? "dark" : "light"));
  system.addEventListener("change", () => {
    if (!stored()) apply(system.matches ? "dark" : "light");
  });
  window.toggleTheme = () => {
    const next = root.dataset.theme === "dark" ? "light" : "dark";
    try {
      localStorage.setItem("diskprism-theme", next);
    } catch {
      // A private window: the choice simply lasts for this page.
    }
    apply(next);
  };
})();
