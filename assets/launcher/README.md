# Launcher header art

`header-art.svg` is the source of truth. Edit it first; its palette lives in the `<style>` block at the top
in a few CSS classes (`.gold` `#E0BB32`, `.amber` `#C9852C`, `.highlight` `#FFE7AE`, `.deep-line` `#8B5A1B`).
Change a class there to restyle the art.

`header-art.png` is the runtime asset, generated from the SVG at 3840x432 (1600x180 viewBox at 2.4x)
with a transparent background. Do not edit it by hand; regenerate it after every SVG change.

From the repository root on Windows, with Edge installed:

```powershell
& 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe' `
  --headless=new --disable-gpu --hide-scrollbars --default-background-color=00000000 `
  --window-size=3840,432 --screenshot=assets/launcher/header-art.png `
  "file:///$((Resolve-Path assets/launcher/header-art.svg).Path -replace '\\','/')"
```

The launcher draws the PNG over its header gradient, anchored to the header's top-right corner at the
full header height; the SVG's empty left side is what keeps the title readable.
