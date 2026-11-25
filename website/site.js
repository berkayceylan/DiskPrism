// Page behavior: theme button, screenshot gallery, copy buttons, demo start-up.

const root = document.documentElement;

// --- theme -----------------------------------------------------------------------------------

const themeButton = document.querySelector("#theme");
const labelTheme = () => themeButton.setAttribute("aria-label", root.dataset.theme === "dark" ? "Switch to light mode" : "Switch to dark mode");
themeButton.addEventListener("click", () => window.toggleTheme());
document.addEventListener("themechange", labelTheme);
labelTheme();

// --- gallery -----------------------------------------------------------------------------------

const gallery = document.querySelector("#gallery");
const shot = { view: "treemap", look: null, frame: "window" }; // look null: follow the page theme
const image = gallery.querySelector("img");
const caption = gallery.querySelector("figcaption");
const CAPTION = {
  window: "The app window, showing a synthetic folder tree.",
  canvas: "Rendered off screen with DiskPrism --snapshot from the same tree.",
};

const sourceOf = ({ view, look, frame }) => `assets/img/${frame === "window" ? "window-" : ""}${view}-${look}`;
function showShot() {
  const current = { ...shot, look: shot.look ?? root.dataset.theme };
  const base = sourceOf(current);
  // The markup already picked the right picture for the system theme; only touch the image
  // when something else is wanted, so nobody downloads a screenshot they never see.
  const untouched = gallery.querySelector("source");
  const system = matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
  if (!untouched || base !== sourceOf({ view: "treemap", look: system, frame: "window" })) {
    untouched?.remove();
    image.srcset = `${base}-1440.webp 1440w, ${base}.webp 2880w`;
    image.src = `${base}-1440.webp`;
  }
  image.alt = `DiskPrism ${current.view} in ${current.look} mode`;
  gallery.dataset.look = current.look;
  caption.textContent = CAPTION[current.frame];
  for (const button of gallery.querySelectorAll("[data-set]")) {
    button.setAttribute("aria-pressed", String(current[button.dataset.set] === button.dataset.value));
  }
}
gallery.addEventListener("click", (event) => {
  const button = event.target.closest("[data-set]");
  if (!button) return;
  shot[button.dataset.set] = button.dataset.value;
  showShot();
});
gallery.addEventListener("pointerover", (event) => {
  const button = event.target.closest("[data-set]");
  if (!button) return;
  const next = { ...shot, look: shot.look ?? root.dataset.theme, [button.dataset.set]: button.dataset.value };
  new Image().src = `${sourceOf(next)}${devicePixelRatio > 1 ? "" : "-1440"}.webp`;
});
document.addEventListener("themechange", () => {
  if (!shot.look) showShot();
});
showShot();

// --- copy buttons ---------------------------------------------------------------------------------

for (const block of document.querySelectorAll(".command")) {
  const button = block.querySelector("button");
  button.addEventListener("click", async () => {
    const text = block.querySelector("code").textContent;
    try {
      await navigator.clipboard.writeText(text);
      button.textContent = "Copied";
    } catch {
      getSelection().selectAllChildren(block.querySelector("code"));
      button.textContent = "Selected";
    }
    setTimeout(() => (button.textContent = "Copy"), 1600);
  });
}

// --- live demo ------------------------------------------------------------------------------------

const rig = document.querySelector("#rig");
let started = false;
async function startDemo() {
  if (started) return;
  started = true;
  try {
    const { start } = await import("./demo/demo.js");
    window.diskprismDemo = await start(rig);
    rig.classList.add("ready");
  } catch (error) {
    console.error(error);
    rig.classList.add("failed");
  }
}
// Load it after the page has painted, or as soon as it is about to scroll into view.
new IntersectionObserver((entries) => entries.some((e) => e.isIntersecting) && startDemo(), { rootMargin: "600px" }).observe(rig);
addEventListener("load", () => setTimeout(startDemo, 1200));
