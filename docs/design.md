# Design

## Goal

The smallest possible memory footprint while mainstream sites keep working.
Every choice is judged by measured memory, with compatibility as the hard
constraint.

The point is a browser that works whenever RAM is tight, not just on old
hardware. That includes a machine where other programs hold most of the
memory, so the browser must react to pressure as it happens, not only fit a
fixed budget. Targets: 2 GB x86-64 laptops (primary) and a 1 GB Raspberry Pi
(arm64).

## Modes

lrb runs in one of two modes; `lrb_coordinator` picks one each time it
starts, from the machine's memory (the setting `mode` can force either):

- **Lean**: everything below. One single-process instance per site, its own
  profile, the whole instance confined (Landlock, seccomp, no_new_privs).
  Chosen when memory is tight: less than 3 GB of RAM, or less than 1.5 GB
  available at start, or no working sandbox for standard mode.
- **Standard**: Chromium's usual process model: zygotes, sandboxed
  renderers locked to their site (site isolation), one browser process for
  every site with one shared profile (`$XDG_DATA_HOME/lrb/standard`), so a
  sign-in at Google or Microsoft serves every site, as in Chrome. The
  bar, tabs (across sites in one window), content blocking, the permission
  model, the file picker, the keyring and the slim defaults that don't
  touch isolation (in-process GPU, network and audio, software compositing,
  no prerendering, the autoplay policy) stay. Dropped: the single process,
  low-end-device mode, V8's size bias and the back-forward cache disable.
  The coordinator still confines the browser process with Landlock (its
  seccomp filter lets Chromium's sandbox create namespaces) and still
  serves lists, bookmarks, the file picker and the keyring.

Where user namespaces are restricted to profiled programs (Ubuntu 23.10
and later, and its derivatives) the tarball and the .deb stay in lean mode:
lrb ships neither an AppArmor profile nor a setuid sandbox helper, so that
nothing it installs reaches outside its own directory. Standard mode there
is for the snap package, whose profile snapd generates (decided
2026-11-02).

Why two: the one-process-per-site model is what makes 1 GB and 2 GB
machines usable, but it costs the shared sign-in, and a desktop with
gigabytes to spare gains nothing from it. The security target in both is
today's standard, Chrome's. In standard mode it is Chrome's model exactly;
in lean mode the table below says where it is stronger and weaker.

Memory: standard mode costs about what Chrome's model costs for the same
pages (MEASUREMENTS.md, "Site per window against Chrome's model": five
sites 1083 MB against 741 MB lean, unreclaimable), minus the Chrome layer.

## How it works (lean mode)

1. **`//content`, not `//chrome`.** The Chrome product layer (sync,
   extensions, Safe Browsing, profile services) costs about 70 MB of
   anonymous memory over content_shell for the same page.
2. **One site per window, one browser process per window.** A window shows
   one site (registrable domain: `google.com` covers `mail.google.com`), and
   each window is its own single-process browser instance with its own
   persistent profile. Users don't have to understand process sharing to be
   safe: sites never share a process or a cookie jar. Five sites cost 32%
   less than Chrome's model.
3. **Single process per instance.** The renderer, compositor, network,
   audio and storage all run in the instance's one process, without zygotes.
4. **Tabs within a site.** Every new page of a site opens as a tab in that
   site's most recently used window: New page, links asking for a new tab,
   and the site opened from elsewhere. Popups that need their opener
   (sign-in, payment) stay small separate windows. A tab opened in the
   background loads only when first shown.
5. **Leaving a site swaps the window and parks the instance.** Typing
   another site's address, or a plain link click to another site, opens
   that site's window where this one is; once it has painted, this window
   hides and its instance stays alive, parked, with its pages, logins and
   state (at most three parked; they are the first to sleep or close under
   pressure, and closing saves their windows). Back from the new window's
   first page shows the parked window again at once. Leaving from a tab of
   several keeps the tab and opens the other site beside it. Links asking
   for a new tab open the other site's window and keep this one. Redirects
   within a flow (OAuth, 3-D Secure, checkout) may pass through other
   sites in the current window: they run in that window's process and
   profile, so they can't reach any other site's data. Session cookies are
   kept and restored for every instance, so a site that closed comes back
   signed in.
6. **Memory pressure.** Parked instances, then background windows, then
   background tabs are discarded in place under pressure (window and
   history kept, reloaded when shown; pages with unsaved changes are
   spared until pressure is critical). Pressure means less than 30% or
   512 MB available, whichever is less (critical: 15% or 256 MB), so a
   desktop with gigabytes free is never under pressure. Only as a last
   resort, still critical with every background window discarded, do the
   oldest windows close; opening the site again restores them with their
   history. Chromium has no memory pressure source on Linux;
   `patches/0001` adds one, `patches/0004` the absolute caps.
7. **A coordinator** (`lrb_coordinator`): a tiny separate process that never
   loads web content. It picks the mode, maps sites to instances, launches
   them (asking `lrb --lrb-site-of` for a URL's site, so no throwaway
   browser starts), restores the sites that were open at the last exit,
   and discards or closes instances under pressure. A hijacked page can't
   steer which windows open, which profiles they get, or what is discarded.
8. **Content blocking** uses Brave's adblock-rust (uBlock Origin-compatible
   lists: surrogates, cosmetic rules, scriptlets), so blocking stays as
   invisible to sites as the community lists keep it. The compiled engine
   is one read-only file shared by every instance: 0.8 MB private per
   instance. Phishing and malware warnings (in place of Safe Browsing)
   use the same engine on public lists of phishing pages and malware
   downloads (malware-filter, uBlock Origin's badware), compiled into a
   file of their own so turning blocking off never turns them off: a
   listed page or download is held before its request
   (`lrb/browser/safety_throttle.h`), with Back and Continue anyway. The
   coordinator refreshes the lists when it starts, never while it runs.
   0.85 MB private per instance; nothing about the pages visited leaves
   the machine.
9. **Window UI:** a slim native (Views) bar with back, forward, reload, a
   site chip in the site's colour with the path beside it (click to edit),
   a blocked-request count and a menu; a second thin row for tabs with two
   or more pages. Not HTML: 6.5 MB against about 2 MB per window.
10. **Permissions:** none without the user saying yes. Camera, microphone
    and clipboard reading are asked about in the window, remembered per
    site and revocable from the menu; everything else is refused. Popups
    need a user gesture. Downloads ask where to save (a setting turns
    that off).
11. **Search:** DuckDuckGo's HTML page by default, changeable in the
    settings.
12. **GPU:** software rendering by default (measured 7-55 MB less per
    window on a Raspberry Pi 3, about 12 MB on a desktop with NVIDIA;
    smoothness hasn't been measured). The GPU is a setting.
13. **Chromium release line:** Stable (what Linux distributions ship),
    rebuilt, tested and compared for memory against the previous release
    for each security release; never main or canary. Chromium's Extended
    Stable has no Linux releases.
14. **Own copy of content_shell.** lrb started from content_shell; the parts
    it uses are copied into `//lrb` and trimmed of web tests, test hooks and
    other platforms. Only genuine Chromium changes live in `patches/`, which
    keeps Stable updates cheap.

## Security model

Standard mode is Chrome's model (sandboxed, site-locked renderers; the
browser process trusted) under Landlock. The table is lean mode.

| Threat | Chrome | lrb (lean mode) |
|---|---|---|
| A site reading another site's data (cookies, storage) | Blocked: data in a privileged process, renderers locked to their site | Blocked: other sites' data is in other processes and profiles |
| A hijacked page reaching your files and system | Per-renderer sandbox | Whole-instance OS confinement (Landlock, seccomp, no_new_privs; `lrb/coordinator/confine.h`) and the V8 sandbox. The user's files reach it only through the coordinator's file picker (`broker.h`) |
| Third-party content (ads, embeds) inside a page | Cross-site iframes in their own process | **Shares the page's process**: a malicious ad on a site can reach that site's data. Mitigated by content blocking |
| One page crashing others | Only its renderer dies | Only its window's instance dies |
| A site learning which other sites you use | History and bookmarks in the browser process, out of renderers' reach | **Readable by any instance** through the coordinator (address-bar suggestions, the bookmarks menu): the sites visited and bookmarks, not what was done there. An instance can add or remove bookmarks of its own site only |
| A hijacked page opening other apps (`mailto:`, `tel:`, meeting links) | Asked in the browser process, out of the renderer's reach | **Asked in the site's instance**, which a hijacked page could skip; the coordinator lets through only links for apps (never web pages or files, `rules.h`), one every 2 s (`lrb/coordinator/coordinator.cc`, OpenInApp) |
| A phishing page or a malware download | Google Safe Browsing: lists on the device, refreshed every 30 minutes, and a check with Google for addresses that match | **Public lists, checked on the device** (malware-filter's phishing and URLhaus lists, uBlock Origin's badware), refreshed when lrb starts: fewer pages caught, and new ones only from the next start. Warned in the site's instance, as certificate errors are |
| A site reading saved passwords and autofill details | In the browser process; filled in by it | **In the desktop's keyring**, through the coordinator (`lrb/coordinator/keyring_broker.h`): an instance gets passwords of its own site only (from the profile the coordinator gave it), never others'. Autofill details (name, address, email, phone) are shared by every site: a hijacked instance can read them without asking |

The remaining exposure is third-party frames inside a site's own window.
Banks and email providers carry few, and content blocking removes most of
the rest.

The coordinator's protocol (`lrb/coordinator/coordinator.cc`) is text lines
over a Unix socket in `$XDG_RUNTIME_DIR/lrb` (mode 0700). It checks that
every URL belongs to the named site and that site names are safe directory
names. A compromised instance can open windows for any site (as a click
could), but it can't make another site's window load a foreign URL or touch
files outside the profiles directory. Bookmark changes are tied to the
profile the coordinator started the instance with (`SO_PEERCRED`), never to
what it says about itself (`lrb/coordinator/bookmarks.h`).

Printing is compiled in for Print to PDF only (`enable_printing`, no print
preview or CUPS; `lrb/browser/print.h`): a page's `window.print()` reaches
Chromium's printing code in the renderer (PrintRenderFrameHelper, Skia's
PDF backend), as in Chrome.
