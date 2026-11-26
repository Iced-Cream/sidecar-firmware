# The machine catalog (web page: Voices > User machines > Catalog)

The catalog lists the user machines and effects of the community registry
(<https://trigcondition.github.io/sidecar-machines/>) and installs one onto the Sidecar from the web page.

## What it is

- A card in the web page, next to the installed user machines. It shows the machines and effects for your board (Pi 4 or Pi 5):
  name, kind, author, version, licence, size, a source link (or "no source"), a demo link when there is one, and the
  description. Filter by kind, or search.
- Each entry says **Install**, **Update to x.y.z**, or **Installed** (compared by id and version). It never offers a
  downgrade. A copy on the card that does not load shows **Reinstall**.
- An update replaces the file already on the card for that machine (also one copied there by hand or from the plugins
  zip, such as `bp303.a64.rmp`); the confirm names the file it replaces. Install waits until the page knows the board.
- An entry that has audio previews (the registry lists up to eight) has a **Previews** toggle with a player; nothing
  plays by itself and nothing is fetched until you press play. The audio streams from the registry's site to your
  browser (so it needs internet, like the rest of the catalog); the Sidecar never touches it. Only `https:` previews
  under the registry's address are offered, and the player stops when you filter, close the list or leave the page.
- Every entry is marked **community**. Entries by trigcondition also say **by Sidecar**; they are installed the same way.

## How it works: the browser does the fetching

- **Your browser needs internet. The Sidecar does not.** The page reads the registry's `index.json` in your browser, and
  downloads the package there. The Pi itself never goes to the internet, so a Sidecar on a USB-C cable to a computer
  with no internet sharing works as before.
- The browser checks the package's size and SHA-256 against the index. A mismatch is refused with a message.
- The package is then sent to the Sidecar through the ordinary upload into `/Plugins`, and the Sidecar's plugin loader
  checks it again, exactly as for any file you upload yourself.
- With no internet in the browser the card says "Catalog not reachable"; nothing else on the page is affected.
- There is one registry address and no setting for it.
- Everything from the index is shown as plain text; only `https:` links are links.

## The trust text

Installing asks once: *a community machine, native code, no sandbox, not tested by Sidecar* (and *no source is published
for it* when it has none). A user machine is native code that runs inside the Sidecar's engine, like a firmware
module: install only what you trust.

## Install by hand instead

Copy the `.rmp` file into `/Plugins` on the card, or use the library page's upload (docs/user-machines-pi.md). The
catalog does the same, with the download and the hash check added.
