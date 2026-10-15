# 32 · Documentation and brand

**Status:** Implemented v2 - edition 02 (25 September 2026) of the User, Installer, Flasher and
Panel Manager Guides, an Astro site in the public repository RoboAlarms/documentation, live at
docs.roboalarms.com; the main website, roboalarms.com, is live. Left: the maintainers' review (BL-017); a vector logo and a PDF
when asked (BL-017); Z-Wave (BL-071) and Spanish (BL-047) in the guides ·
**Milestone:** docs · **Depends on:** 07, 14, 23, 31, 34 (and every feature the guides
describe; [UPDATING.md](https://github.com/RoboAlarms/documentation/blob/main/UPDATING.md) §1)

**Code:** [RoboAlarms/documentation](https://github.com/RoboAlarms/documentation) (the site, its capture and check scripts) ·
[`docs/BRAND.md`](../../BRAND.md) (the logo) · **Procedure:**
[UPDATING.md](https://github.com/RoboAlarms/documentation/blob/main/UPDATING.md)

## Objective and scope

Create professional RoboAlarms User and Installer Guides in the public repository RoboAlarms/documentation, using Astro
Starlight (since edition 02 also separate Flasher and Panel Manager Guides). The Installer
Guide covers software setup and commissioning only: physical hardware, mounting, wiring and
hardware purchasing instructions are out of scope. PDF production is explicitly deferred.
Documentation is an on-request snapshot, not a mandatory update accompanying every
software change. Keep the internal feature and bug records current under the existing project
rules.

## Editorial requirements

- Name controls exactly as the actual UI does; explain prerequisites, steps, expected
  result and recovery. Prefer one topic per page and descriptive searchable titles.
- Distinguish implemented functions from planned or hardware-dependent ones. Avoid
  claims of certification, monitoring dispatch or production acceptance.
- No personal identities, codes, addresses or network names in public assets.
- Keep screenshots in the documentation repository; software preview regeneration must not
  silently replace a published manual's screenshots.
- On an explicit documentation refresh, record the reviewed software commit, edition,
  capture date and checks. No per-firmware-change documentation gate or auto-publishing.

## Design

Where an edition lives and how it is made; the steps are in [UPDATING.md](https://github.com/RoboAlarms/documentation/blob/main/UPDATING.md), the
commands and conventions in [README.md](https://github.com/RoboAlarms/documentation/blob/main/README.md).

- The site: the root of RoboAlarms/documentation, an Astro Starlight package with its own lockfile. Pages in
  `src/content/docs/` under `user/`, `installer/`, `flasher/`, `panel-manager/` and
  `reference/`; the edition, its reviewed source commits and its coverage in
  `reference/edition.mdx` and the footer (`src/components/DocsFooter.astro`).
- Panel screenshots: the host UI preview with fictional data, through
  `scripts/capture-panel.ps1` (it sets non-default codes after every simulated restart) and
  `scripts/curate-images.mjs`. Browser tools: their mocks, through
  `scripts/stage-panel-manager.mjs` and `scripts/curate-browser-images.mjs`. Home Assistant: an
  isolated lab with a fictional panel (`scripts/ha-lab/README.md`, `scripts/curate-ha-images.mjs`).
  Crops, hashes and provenance in `image-manifest.json`, `browser-image-manifest.json` and
  `home-assistant-manifest.json`.
- Publishing: `site` in `astro.config.mjs` is `https://docs.roboalarms.com`, served at the root.
  A local build publishes nothing; there is no push-triggered deployment.
- The brand: `docs/BRAND.md` (the mark, its teal accents and its use).

## Test and acceptance

From the documentation repository ([UPDATING.md](https://github.com/RoboAlarms/documentation/blob/main/UPDATING.md) §5):
- `pnpm check` (Astro) and `pnpm build`;
- `node scripts/verify-site.mjs`: every local link, anchor and asset in the built site;
- `node scripts/verify-images.mjs`: each curated image against its manifest (dimensions, hash,
  not blank);
- `git diff --check` from the repository root;
- a review in the browser (`pnpm preview --port 4321`): desktop and 390/320 px, light and dark,
  the menus, the images and the production search. A passing build is not editorial acceptance.

The panel fixture fails the capture if a factory code is still in use (`default_codes == 0`
before every capture). An edition's checks are not hardware acceptance: the guides'
hardware-dependent commissioning steps stay untested until the panel checks in section 3 of
[the backlog](../../BACKLOG.md) have run.

## Open questions

1. When later editions are published, after a review of their wording, navigation
   and logo (BL-017). Decided: the address is docs.roboalarms.com, at the site root
   (`astro.config.mjs` in RoboAlarms/documentation, `05852fd`), served through Cloudflare; it serves edition 02
   and later editions go there after the review.
2. The logo (a teal geometric R and house) is part of that review (BL-017). Decided
   2026-09-28: it is the identity (it is on the live website and guides); the vector master
   (`docs/BRAND.md`, BL-017) is made when asked.
3. A PDF or print edition: deferred since edition 01, made only when asked (BL-017).

## Tasks

- [x] Edition 01: the User and Installer Guides, panel and Home Assistant screenshots, the logo
      (`8ef0da6`)
- [x] Edition 02: every page refreshed, the Flasher, partitions and newer settings, screenshots
      with non-default codes, the shared procedure [UPDATING.md](https://github.com/RoboAlarms/documentation/blob/main/UPDATING.md) (`05852fd`)
- [x] Separate Flasher and Panel Manager Guides, the old installer URLs kept (`21a0cbe`)
- [x] The guides' domain chosen and Astro `site` set, no `base` (`astro.config.mjs`)
- [ ] The maintainers review the guides' wording, navigation and logo direction in the local preview
      (BL-017)
- [x] Edition 02 published at docs.roboalarms.com, through Cloudflare (its host's setup is not
      recorded in the repository)
- [ ] A vector master of the logo, once the identity is settled (`docs/BRAND.md`)
      (BL-017)
- [ ] A PDF/print workflow, when requested (BL-017)
- Z-Wave in the guides at the next edition: duplicate of docs/features/36 (Z9, "builder docs
  when asked"), BL-071.
- A Spanish edition, through Starlight's multilingual support with screenshots from the preview
  in Spanish: duplicate of [Globalization](../37-Globalization/README.md) G7 ("A Spanish edition
  of the guides", docs/features/37), BL-047; it adds a language step to
  [UPDATING.md](https://github.com/RoboAlarms/documentation/blob/main/UPDATING.md).
