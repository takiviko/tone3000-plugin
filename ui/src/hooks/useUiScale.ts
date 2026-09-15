import { useLayoutEffect } from 'react';

/** Design-space width of the plugin UI. Native sizes the window to this
 * times the user's scale factor (see TONE3000Editor in plugin/include). */
export const DESIGN_WIDTH = 1024;
/** Design-space height of the plugin UI (content only). Figma's 600px
 * artboard includes a 22px mock OS title bar outside JUCE setSize. */
export const DESIGN_HEIGHT = 578;

// Live design-space height of the root box: DESIGN_HEIGHT plus whatever
// chrome strips (banner, hint bar) are currently mounted. Published by
// useChromeChoreography via setUiDesignHeight below.
let committedDesignHeight = DESIGN_HEIGHT;

// A larger box height waiting for the native window resize to land before
// it's allowed to shrink the scale; see setUiDesignHeight.
let pendingDesignHeight: number | undefined;
let pendingTimer: number | undefined;

/**
 * True in the iOS app, where native injects the flag at document start (see
 * the JUCE_IOS user script in EditorWebViewSetup.cpp). Gates iOS-specific
 * behavior that has nothing to do with window sizing: the Bluetooth-HFP
 * sample-rate tip in AppBanner (iOS-only IosAudioRoute workaround) and the
 * WKWebView touch-hold-as-contextmenu shim in GalleryBlock/useTouchHold
 * (WKWebView never fires a native contextmenu for a touch hold; unverified
 * whether Android's WebView needs the same treatment, so it isn't
 * extended there). For the fixed-window layout handling both platforms
 * actually share, see IS_FIXED_WINDOW below - don't add new iOS-only
 * behavior here without checking whether it's really iOS-only.
 */
export const IS_IOS =
  (window as unknown as { __T3K_PLATFORM__?: string }).__T3K_PLATFORM__ === 'ios';

/** True in the Android app (see the JUCE_ANDROID user script in
 * EditorWebViewSetup.cpp). */
export const IS_ANDROID =
  (window as unknown as { __T3K_PLATFORM__?: string }).__T3K_PLATFORM__ === 'android';

/**
 * True in any build whose window is a fixed, full-screen box: there is no
 * corner drag, no host resize request, and the screen's aspect is nothing
 * like the design box's 1024:578. iOS and Android both fit this (a
 * fullscreen Activity has the same characteristics as iOS's fixed window);
 * every desktop build does not. Gates the document-scroll fix and vertical
 * centering in index.css (via the t3k-fixed-window class below).
 *
 * iOS and Android hit different shapes of the same underlying class of bug
 * here, confirmed independently on each: WKWebView (iOS) resolves `height:
 * 100%`/`100vh` to a slightly *wrong* definite value (a 25px
 * clientHeight/scrollHeight mismatch - see index.css), which the CSS
 * percentage chain alone fixes. Android's WebView is worse: confirmed live
 * via Chrome DevTools Protocol against the running WebView (remote
 * debugging is already on - JuceWebView calls
 * setWebContentsDebuggingEnabled(true) unconditionally; forward the socket
 * with `adb forward tcp:<port> localabstract:webview_devtools_remote_<pid>`,
 * found via `adb shell cat /proc/net/unix | grep devtools`) that `height:
 * 100%`/`100vh` resolve to a literal 0px on Android, even inside a fresh
 * `position: fixed` test element with no ancestor involvement, and even
 * though window.innerHeight/visualViewport.height/Page.getLayoutMetrics()
 * all correctly report the true viewport size throughout. An *explicit
 * pixel* height works correctly on the same elements. So the CSS percentage
 * chain alone (sufficient for iOS) is not enough for Android - see the
 * applyAndroidExplicitPixelHeight side effect below, which is the
 * JS-computed-pixel-height workaround this Blink/WebView bug actually
 * requires.
 */
export const IS_FIXED_WINDOW = IS_IOS || IS_ANDROID;

// Stylesheet hook for the fixed-window rules in index.css (the
// document-scroll fix and the vertical centering). Set here rather than in
// a component so it is on the element before the first paint, and set only
// where needed, so every desktop build's <html> carries no extra class.
if (IS_FIXED_WINDOW && typeof document !== 'undefined')
  document.documentElement.classList.add('t3k-fixed-window');

// Android-only workaround for the percentage/vh-resolves-to-0 bug described
// on IS_FIXED_WINDOW above: index.css's html/body height:100% rules can't
// work on their own here, so set an explicit pixel height matching the real
// (correctly-reported) viewport instead, and keep it in sync on resize.
// Runs before first paint like the class-toggle above, not gated behind a
// React effect. iOS is left on the pure-CSS fix (confirmed sufficient
// there); revisit only if a case turns up where it isn't.
if (IS_ANDROID && typeof document !== 'undefined' && typeof window !== 'undefined') {
  const applyAndroidExplicitPixelHeight = () => {
    const px = `${window.innerHeight}px`;
    document.documentElement.style.height = px;
    document.body.style.height = px;
  };
  applyAndroidExplicitPixelHeight();
  window.addEventListener('resize', applyAndroidExplicitPixelHeight);
}

/**
 * True when the primary pointer is a finger (iPad, Android and Windows
 * tablets). Gates the static touch ergonomics: the 44 pt hit floor and the
 * touch-field growth in index.css (via the t3k-touch class below), the touch
 * help copy, and render-time nudges that follow them. Behaviors gate on each
 * event's own pointerType instead, so a hybrid device gets touch behavior
 * from its touchscreen and desktop behavior from its mouse.
 */
export const IS_COARSE_POINTER = window.matchMedia('(pointer: coarse)').matches;

if (IS_COARSE_POINTER && typeof document !== 'undefined')
  document.documentElement.classList.add('t3k-touch');

/** Largest scale at which a 1024 x designHeight box fits the viewport. The
 * floor covers 0-sized viewports during boot/teardown: pointer math divides
 * by the scale, so it must never be 0. */
const fitScale = (designHeight: number): number => {
  const el = document.documentElement;
  return Math.max(0.05, Math.min(el.clientWidth / DESIGN_WIDTH, el.clientHeight / designHeight));
};

/** Current UI scale (real viewport px per design px): the design box fitted
 * to the actual viewport, letterboxed (#root centers the box, spare space is
 * black-on-black). Deliberately not floored at 1x: the viewport can be
 * genuinely smaller than the design box, e.g. WebView2 rasterizing at the OS
 * display scale inside a host that sized the window in raw physical px
 * (Pro Tools <= 12 on Windows), or a WM breaking the aspect lock
 * (GNOME/Wayland). Fitting down keeps the whole UI visible: no scrollbars,
 * nothing clipped. */
export const getUiScale = (): number => fitScale(committedDesignHeight);

/** A design-space length as a CSS value. The root font-size is `scale`px
 * (useUiScale), so `1rem` is exactly one design px at the current scale:
 * rem(13) at 1.5x renders 19.5 real px. */
export const rem = (designPx: number): string => `${designPx}rem`;

const applyUiScale = (): void => {
  document.documentElement.style.setProperty('font-size', `${getUiScale()}px`);
};

const commitPendingDesignHeight = (): void => {
  if (pendingTimer !== undefined) {
    window.clearTimeout(pendingTimer);
    pendingTimer = undefined;
  }
  if (pendingDesignHeight !== undefined) {
    committedDesignHeight = pendingDesignHeight;
    pendingDesignHeight = undefined;
    applyUiScale();
  }
};

// Committing `pending` now keeps the scale within rounding noise of the
// committed one (native setSize rounds to whole px; 0.5% is well under a
// visible change), i.e. the window has grown to make room for the bigger box.
const pendingFitsWithoutShrink = (): boolean =>
  pendingDesignHeight !== undefined && fitScale(pendingDesignHeight) >= getUiScale() * 0.995;

/**
 * Publish the design-space height of the root box (the 578px core plus the
 * mounted chrome strips): the fit must track the real box, or a strip would
 * clip in a window that can't grow.
 *
 * A taller box doesn't shrink the scale right away. The commit that mounts a
 * strip also asks native for a taller window (setExtraContentHeight), but
 * that resize is async across the bridge, and fitting the taller box against
 * the still-short viewport would pulse the whole UI down and back. So the
 * new height commits once the viewport has grown enough that the scale
 * holds (checked on every resize), or after a timeout when the host refused
 * the resize: then shrinking to fit is the correct end state. A shorter box
 * commits immediately, the scale can only stay or grow.
 */
export function setUiDesignHeight(designHeight: number): void {
  if (designHeight === (pendingDesignHeight ?? committedDesignHeight)) return;
  pendingDesignHeight = designHeight;
  if (pendingFitsWithoutShrink()) {
    commitPendingDesignHeight();
    return;
  }
  if (pendingTimer !== undefined) window.clearTimeout(pendingTimer);
  // Matches the banner choreography's give-up window for hosts that refuse
  // or delay resizes (see useChromeChoreography).
  pendingTimer = window.setTimeout(commitPendingDesignHeight, 450);
}

/**
 * Proportional UI scaling: sets the root font-size to `scale`px (with the
 * scale fitting the design box into the viewport, see getUiScale) so every
 * rem-denominated length in the UI (knobs, fonts, spacing, the whole design
 * space is authored in rem, one rem per design px) tracks the window. Unlike
 * CSS `zoom`, layout runs directly at the real size: text and SVG rasterize
 * crisp, scrolling composites normally, and pointer coordinates, element
 * rects, and position:fixed all agree in real viewport px on every engine
 * (no pointer patching, no portal special cases).
 *
 * The scale follows the *actual* viewport, never a requested size: a host
 * that refuses a resize leaves the viewport (and therefore the scale)
 * untouched.
 *
 * Deliberately imperative (no React state): a live window drag retunes the
 * font-size every frame without re-rendering the tree. ResizeObserver on
 * <html> rather than window `resize`, which fires inconsistently across
 * WebView2 / WKWebView / WebKitGTK. clientWidth/Height are unaffected by
 * font-size, so no feedback loop is possible.
 *
 * External pages (the TONE3000 select flow) replace this document entirely
 * and render with their own styles, using the larger viewport responsively.
 */
export function useUiScale(): void {
  useLayoutEffect(() => {
    const apply = () => {
      // A pending taller box commits as soon as the window's resize lands.
      if (pendingFitsWithoutShrink()) commitPendingDesignHeight();
      else applyUiScale();
    };

    apply();
    const observer = new ResizeObserver(apply);
    observer.observe(document.documentElement);
    return () => observer.disconnect();
  }, []);
}
