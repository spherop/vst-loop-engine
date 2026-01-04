# Theming System Architecture Plan

## Overview

This document outlines the plan for a CSS-based theming system that allows Loop Engine to ship with multiple visual themes and eventually support user-created themes. The system consolidates the currently scattered color definitions (Tailwind config, CSS variables, JS arrays) into a unified architecture with JSON manifests and CSS variable overrides.

---

## Current Architecture

### Color Definition Locations (Scattered)

**1. Tailwind Config** (`ui/index.html` lines 9-21):
```javascript
colors: {
  'fd-bg': '#0a0a0a',
  'fd-surface': '#111111',
  'fd-card': '#161616',
  'fd-border': '#2a2a2a',
  'fd-accent': '#ff6b35',
  'fd-accent-dim': '#cc5429',
  'fd-text': '#888888',
  'fd-text-dim': '#555555',
  'fd-highlight': '#ff8c5a',
}
```

**2. CSS Variables** (`ui/styles.css` lines 6-35):
```css
--looper-accent: #4fc3f7;
--delay-accent: #ff6b35;
--lofi-accent: #66bb6a;
--granular-accent: #ab47bc;
--reverb-accent: #26c6da;
--saturation-accent: #ff9100;
/* each with -dim and -glow variants */
```

**3. JavaScript Layer Colors** (`ui/main.js` - duplicated 3 times):
```javascript
this.layerColors = [
  '#4fc3f7',  // Layer 1: Cyan
  '#ff7043',  // Layer 2: Deep orange
  '#66bb6a',  // Layer 3: Green
  '#ab47bc',  // Layer 4: Purple
  '#ffa726',  // Layer 5: Orange
  '#26c6da',  // Layer 6: Teal
  '#ec407a',  // Layer 7: Pink
  '#9ccc65',  // Layer 8: Light green
];
```

**4. Hardcoded Values**:
- ~184 raw hex codes scattered throughout `styles.css`
- Canvas backgrounds: `#0a0a0a`, `#1a1a1a`
- Text colors: `#333`, `#555`, `#888`

### Current Color Usage

| Location | Usage |
|----------|-------|
| Tailwind `fd-*` | Background, surface, borders, text via utility classes |
| CSS `--*-accent` | Effect panel accents, buttons, glows |
| JS `layerColors[]` | Canvas waveform fill/stroke, layer button inline styles |

---

## Theme Structure Design

### Theme File Organization

```
ui/
  themes/
    ThemeManager.js        # Theme loading, switching, persistence
    themes-manifest.json   # Index of built-in themes
    default/
      theme.json           # Metadata + layer colors
      theme.css            # CSS variable definitions
    neon/
      theme.json
      theme.css
    monochrome/
      theme.json
      theme.css
```

### Theme Manifest Format (`theme.json`)

```json
{
  "id": "default",
  "name": "Midnight",
  "version": "1.0.0",
  "author": "Loop Engine Team",
  "description": "Default dark theme with orange accents and colorful layers",
  "layerColors": [
    "#4fc3f7",
    "#ff7043",
    "#66bb6a",
    "#ab47bc",
    "#ffa726",
    "#26c6da",
    "#ec407a",
    "#9ccc65"
  ],
  "previewColors": {
    "background": "#0a0a0a",
    "accent": "#ff6b35",
    "looper": "#4fc3f7"
  }
}
```

- **`layerColors`**: Explicit array of 8 hex colors for waveforms/layer buttons
- **`previewColors`**: Quick reference for theme selector swatches

### Theme CSS Format (`theme.css`)

Consolidated CSS custom properties using `--le-*` namespace:

```css
:root {
  /* === Core UI Colors === */
  --le-bg: #0a0a0a;
  --le-bg-surface: #111111;
  --le-bg-card: #161616;
  --le-bg-elevated: #1a1a1a;

  /* === Border Colors === */
  --le-border: #2a2a2a;
  --le-border-dim: #222222;
  --le-border-bright: #333333;

  /* === Text Colors === */
  --le-text: #888888;
  --le-text-dim: #555555;
  --le-text-bright: #cccccc;
  --le-text-on-accent: #0a0a0a;

  /* === Primary Accent (UI chrome) === */
  --le-accent: #ff6b35;
  --le-accent-dim: #cc5429;
  --le-accent-glow: rgba(255, 107, 53, 0.4);

  /* === Looper Accent (transport, playhead) === */
  --le-looper: #4fc3f7;
  --le-looper-dim: #29b6f6;
  --le-looper-glow: rgba(79, 195, 247, 0.4);

  /* === Effect Accents === */
  --le-delay: #ff6b35;
  --le-delay-dim: #cc5429;
  --le-delay-glow: rgba(255, 107, 53, 0.4);

  --le-lofi: #66bb6a;
  --le-lofi-dim: #558b4b;
  --le-lofi-glow: rgba(102, 187, 106, 0.4);

  --le-granular: #ab47bc;
  --le-granular-dim: #8e24aa;
  --le-granular-glow: rgba(171, 71, 188, 0.4);

  --le-reverb: #26c6da;
  --le-reverb-dim: #00acc1;
  --le-reverb-glow: rgba(38, 198, 218, 0.4);

  --le-saturation: #ffb74d;
  --le-saturation-dim: #ffa726;
  --le-saturation-glow: rgba(255, 183, 77, 0.4);

  /* === Layer Mode === */
  --le-mode-layer: #00bcd4;
  --le-mode-layer-glow: rgba(0, 188, 212, 0.4);

  /* === Canvas Colors (JS reference) === */
  --le-canvas-bg: #0a0a0a;
  --le-canvas-grid: #1a1a1a;
}
```

### Built-in Themes Manifest (`themes-manifest.json`)

```json
{
  "builtInThemes": [
    { "id": "default", "name": "Midnight", "path": "default/" },
    { "id": "neon", "name": "Neon", "path": "neon/" },
    { "id": "monochrome", "name": "Monochrome", "path": "monochrome/" }
  ],
  "defaultTheme": "default"
}
```

---

## ThemeManager Design

### Class Structure

```javascript
class ThemeManager {
  constructor() {
    this.currentTheme = null;
    this.builtInThemes = new Map();   // id → theme data
    this.userThemes = new Map();       // id → theme data
    this.layerColors = [];             // Current 8 layer colors
    this.themeChangeListeners = [];
  }

  // Initialization
  async init()
  async loadBuiltInThemes()
  async loadUserThemes()
  async restoreSelectedTheme()

  // Theme switching
  async applyTheme(themeId)
  injectThemeCSS(cssContent)
  injectLayerColorVariables()
  updateTailwindColors()

  // For canvas code
  getLayerColors()        // Returns array of 8 hex strings
  getLayerColor(index)    // Returns single color

  // Event system
  onThemeChange(callback)
  notifyListeners()

  // Persistence
  saveSelectedTheme(themeId)

  // Theme enumeration
  getAvailableThemes()
}
```

### Theme Application Flow

1. Load theme manifest (`theme.json`) and CSS (`theme.css`)
2. Inject CSS into `<style id="theme-styles">` element
3. Update `layerColors` array from manifest
4. Inject layer color CSS variables (`--le-layer-1` through `--le-layer-8`)
5. Update Tailwind config to reference CSS variables
6. Notify listeners (triggers canvas redraws)
7. Persist selection to localStorage

### Layer Colors: Hybrid CSS + JS Approach

Since canvas drawing requires direct hex values in JavaScript:

1. **JSON manifest** defines the 8 layer colors
2. **ThemeManager** loads them into `this.layerColors` array
3. **CSS variables** injected for DOM-based styling:
   ```javascript
   this.layerColors.forEach((color, i) => {
     document.documentElement.style.setProperty(`--le-layer-${i + 1}`, color);
   });
   ```
4. **Canvas code** uses `themeManager.getLayerColor(layerIdx)`

---

## Storage Architecture

### Built-in Themes
- Embedded in plugin via CMake file glob
- Located in `ui/themes/*/`
- Compiled into BinaryData

### User Themes (Phase 3)
- Located in `~/Library/Application Support/LoopEngine/themes/`
- Same structure as built-in themes
- Discovered at runtime via native function bridge

### Persistence
- Selected theme ID stored in `localStorage.getItem('loopEngine.theme')`
- Fallback to `"default"` if not set

---

## UI Integration

### Theme Selector Location

Settings button in header area (gear icon near version ticker):

```
┌─────────────────────────────────────────────────────────────┐
│  LOOP ENGINE                                    [⚙] v11.3.1 │
└─────────────────────────────────────────────────────────────┘
```

### Settings Panel

Flyout panel with theme dropdown:

```
┌─────────────────────────────┐
│  SETTINGS                 ✕ │
├─────────────────────────────┤
│  THEME                      │
│  ┌───────────────────────┐  │
│  │ Midnight            ▼ │  │
│  └───────────────────────┘  │
│                             │
│  ┌─────────────────────┐    │
│  │ ████ ████ ████ ████ │    │  ← Layer color preview
│  └─────────────────────┘    │
│                             │
│  [Open Themes Folder]       │  ← Phase 3
└─────────────────────────────┘
```

### Live Preview

- Hover over dropdown option → temporarily apply theme
- Select option → persist theme
- Blur/cancel → revert to previous

---

## Migration Strategy

### Phase 1: Infrastructure + Default Theme

1. **Create ThemeManager.js** with core functionality
2. **Extract current colors** into `default/theme.json` and `default/theme.css`
3. **Update styles.css**: Replace hardcoded hex with `var(--le-*, fallback)`
4. **Update index.html**: Tailwind colors reference CSS variables
5. **Update main.js**:
   - Remove 3 duplicate `layerColors` arrays
   - Use `themeManager.getLayerColors()` instead
   - Subscribe to theme changes for canvas redraws
6. **Update CMakeLists.txt**: Add `ui/themes/` to file glob

### Backwards Compatibility

During migration, define legacy mappings:

```css
:root {
  /* Legacy → New mappings */
  --looper-accent: var(--le-looper, #4fc3f7);
  --delay-accent: var(--le-delay, #ff6b35);
  --lofi-accent: var(--le-lofi, #66bb6a);
  /* etc. */
}
```

Tailwind config with fallbacks:

```javascript
colors: {
  'fd-bg': 'var(--le-bg, #0a0a0a)',
  'fd-surface': 'var(--le-bg-surface, #111111)',
  // etc.
}
```

---

## Implementation Phases

### Phase 1: Core Infrastructure

**Goal**: Single working theme, all colors unified

| Task | Files |
|------|-------|
| Create ThemeManager.js | `ui/themes/ThemeManager.js` (new) |
| Create default theme manifest | `ui/themes/default/theme.json` (new) |
| Create default theme CSS | `ui/themes/default/theme.css` (new) |
| Create themes manifest | `ui/themes/themes-manifest.json` (new) |
| Migrate styles.css to CSS vars | `ui/styles.css` |
| Update Tailwind config | `ui/index.html` |
| Integrate ThemeManager in main.js | `ui/main.js` |
| Add themes to build | `CMakeLists.txt` |

### Phase 2: Theme Switching UI + More Themes

**Goal**: Users can switch between built-in themes

| Task | Files |
|------|-------|
| Add settings button to header | `ui/index.html` |
| Create settings panel HTML/CSS | `ui/index.html`, `ui/styles.css` |
| Wire up theme selector | `ui/main.js` |
| Create "Neon" theme | `ui/themes/neon/` (new) |
| Create "Monochrome" theme | `ui/themes/monochrome/` (new) |
| Live preview on hover | `ui/main.js` |

### Phase 3: User Themes Support

**Goal**: Users can add custom themes

| Task | Files |
|------|-------|
| Add `listUserThemes` native function | `src/PluginEditor.cpp` |
| Add `readUserThemeFile` native function | `src/PluginEditor.cpp` |
| Scan user themes directory | `ui/themes/ThemeManager.js` |
| "Open Themes Folder" button | `ui/main.js` |
| Theme authoring documentation | `docs/CREATING-THEMES.md` (new) |

---

## Example Built-in Themes

### Midnight (Default)
- **Background**: Pure black (`#0a0a0a`)
- **Primary accent**: Warm orange (`#ff6b35`)
- **Looper accent**: Cyan (`#4fc3f7`)
- **Feel**: Dark, warm, professional

### Neon
- **Background**: Dark gray (`#121212`)
- **Primary accent**: Hot pink (`#ff0080`)
- **Looper accent**: Electric blue (`#00ffff`)
- **Layer colors**: High saturation, vibrant
- **Feel**: Retro-futuristic, energetic

### Monochrome
- **Background**: True black (`#000000`)
- **Primary accent**: White (`#ffffff`)
- **Looper accent**: Light gray (`#cccccc`)
- **Layer colors**: Grayscale gradient (white → light gray)
- **Feel**: Minimal, focused, accessible

---

## CSS Variable Naming Convention

| Prefix | Usage | Example |
|--------|-------|---------|
| `--le-bg` | Background colors | `--le-bg`, `--le-bg-surface`, `--le-bg-card` |
| `--le-border` | Border colors | `--le-border`, `--le-border-dim` |
| `--le-text` | Text colors | `--le-text`, `--le-text-dim`, `--le-text-bright` |
| `--le-accent` | Primary UI accent | `--le-accent`, `--le-accent-dim`, `--le-accent-glow` |
| `--le-looper` | Looper/transport | `--le-looper`, `--le-looper-glow` |
| `--le-{effect}` | Effect accents | `--le-delay`, `--le-lofi`, `--le-granular`, etc. |
| `--le-layer-N` | Layer colors 1-8 | `--le-layer-1`, `--le-layer-2`, etc. |

---

## Open Questions

1. Should theme selection be saved to native settings (cross-instance sync) or just localStorage?
2. Should we support theme hot-reloading for development? (Watch user themes folder for changes)
3. Do we want themed scrollbars or keep them neutral?
4. Should effect panel colors (LOFI green, REVERB cyan, etc.) be themeable or stay consistent for recognition?

---

## Next Steps

- [ ] Review this plan and confirm Phase 1 scope
- [ ] Audit styles.css for all hardcoded colors that need migration
- [ ] Create ThemeManager.js skeleton
- [ ] Extract current colors into default theme files
- [ ] Prototype CSS variable injection and canvas redraw cycle
