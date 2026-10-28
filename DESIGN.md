# DESIGN.md

Direction for the HD60 S Capture app and the HD60 S Native OBS source.
This is the direction. `antislop.md` is the filter applied on top of it.

## Identity

A utility for one job: get a console's HDMI picture onto a Mac, and show
honestly what the signal chain is doing. It is a tool, not a product page.
It sits next to OBS and QuickTime, so it should look like it belongs to macOS
rather than announcing itself.

Chosen direction: **macOS native, quiet.** Follows Apple conventions closely,
restrained, system fonts and materials, blends into the OS.

## Dials

| Dial | Value | Why |
|---|---|---|
| ENERGY | 1 | A capture utility is background furniture. The console's picture is the content; the app must not compete with it. |
| RHYTHM | 1 (status frames), 2 (app) | Five status states must be recognizable as the same screen, so they share one composition. The app window has two distinct zones (picture, controls) and varies between them. |
| MOTION | 1 | State changes only: a control bar fade, a progress spinner. Nothing moves on a loop. |

## Identity motif: the signal chain

One motif, used in the logo, the status frames, and the app: the path the
picture takes, drawn as three nodes joined by two segments.

```
Source ──HDMI── HD60 S ──USB── Mac
```

It is the motif **and** the diagnostic. Each failure state breaks the chain at
the point that actually failed, so the picture tells you where to look before
you read a word:

| State | Break point |
|---|---|
| Starting up | nothing broken, chain not yet carrying signal |
| No capture card | the USB segment, between card and Mac |
| No signal | the HDMI segment, between source and card |
| Card in use | the card node, held by something else |
| Capture interrupted | the card node, failed |

Reason it earns its place: it carries information no label does (which link
failed), it is specific to this product, and it is the same mark as the logo,
so the app and the OBS source read as one thing.

## Palette

Neutral base plus two accents. Only one accent is ever on screen at a time,
because the accent encodes the severity of the current state.

| Token | Value | Use |
|---|---|---|
| Surface | `#121214` | Status frame and app background. Flat, not a gradient: it sits inside an OBS scene, where a designed-looking background would read as content rather than as the absence of picture. |
| Text primary | `#F5F5F7` | Titles. |
| Text secondary | `#A1A1A8` | Detail lines, node labels. |
| Chain neutral | `#55555E` | Working segments and nodes. |
| Attention | `#E8A33D` | Waiting on something the user can fix: no signal, no card, card in use. |
| Fault | `#E5484D` | Capture failed. |

## Logo

The same chain, reduced to its middle: a lead in, the card, a lead out. The
accent runs *through* the card at the same weight as the leads, because that is
what the card does to the signal, and because an icon with no color is hard to
find in a Dock. Below 32px the leads are dropped and the card carries the mark
alone. Generated from source by `tools/make-icon.c`, so it is reproducible and
reviewable rather than a binary nobody can edit.

The app's own controls use AppKit's semantic colors rather than hand-picked
ones: `systemGreen` for the live indicator, `systemRed` while recording,
`secondaryLabelColor` for inactive text. They already track the system theme,
so there is nothing to maintain and nothing to get wrong in a future macOS.

## Typography

System font (SF Pro) at system weights, via `CTFontCreateUIFontForLanguage` and
`NSFont`. Reason: the direction is macOS native, so the OS typeface is the
correct answer rather than a default pick. Monospaced digits for anything that
counts (resolution, frame rate, elapsed time, dB) so numbers do not jitter.

No uppercase letter-spaced labels. No monospace used as decoration.

## Owner override, 2026-10-27

The owner asked for the macOS 26 glass language: translucent materials, depth,
specular highlights, layered shadows, and an ambient status screen. That collides
with antislop R-10 (glass capped at one or two elements) and R-13 (glow as a one
or two element accent). Kept, on the owner's decision. The purpose that makes it
more than decoration: macOS 26 made glass the system material, so using
`NSGlassEffectView` is now the native answer rather than a trend, which is the
direction this file already set. Glass still goes on surfaces that float above
content (the control bar), never on the picture itself.

## Owner override, 2026-10-27 (evening)

The owner asked for an app that feels like a piece of capture hardware rather
than a generic window, and read the previous result as flat. ENERGY moves 1 to
2 and RHYTHM 1 to 2 for the app window; the status frames keep ENERGY 1 and
RHYTHM 1, because they stand in for a picture inside someone else's scene.

What that bought, and why each is more than decoration:

- The header carries the app mark, the name, and a one line note about the card,
  so the window says which device it belongs to before it says anything else.
- The input's facts are three chips rather than one run-together line. They are
  three separate facts: a console can change resolution while the audio rate
  holds, and a single line hides which one moved.
- The control bar is three groups with separators: what is being saved, what is
  playing, how the picture is shown. The grouping is the hierarchy.
- Settings moved from a popover to a window with Audio, Video, Recording and
  Capture, the shape a capture tool is expected to have. The old popover's
  uppercase section headers are gone with it, which the typography rule above
  ruled out anyway.

## What this direction rules out

- Flat, lifeless surfaces with no depth, now that the override above applies. Ambient falloff and specular edges are in; saturated colored halos behind everything are still out.
- Icons drawn in the generic thin-stroke rounded library style.
- An eyebrow label above a headline repeating what the headline says.
- A status dot that marks no state. The app's LIVE dot marks a real state and stays.

## Dark only, deliberately

Both surfaces sit around moving video: the app wraps a preview, the status
frame replaces one inside an OBS scene. A light surface next to video tints
perception of the picture and glares in a dark room, which is why video tools
are dark. This is a reason, not a default, so there is no light mode.
