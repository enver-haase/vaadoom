# Publishing to the Vaadin Directory — the real, working procedure

This documents the **actual** end-to-end procedure for publishing a Vaadin 25.2 Flow add-on to the
[Vaadin Directory](https://vaadin.com/directory), as verified for Vaadoom on 2026-07-20, plus the
gaps between what the official docs / Vaadin MCP say and what the Directory UI actually requires.

## TL;DR — what actually works

1. Build the ZIP: `mvn clean install -Pdirectory` → `target/vaadoom-<version>.zip`.
2. Sign in at `vaadin.com/directory`.
3. Go to **Publish component** → `https://vaadin.com/directory-edit/my-components`.
4. Click **New component** → **Upload File...** → pick the ZIP. This parses the manifest and opens
   the component editor at `/directory-edit/component/edit/<artifactId>`.
5. On the **Description** tab fill **all** of: **Summary** (>5 chars), **Description** (>20 chars),
   at least one **Category**, and tick **Published**.
6. **Switch to the `Versions` tab, click `Edit` on the version, and fill the version-level required
   fields** — this is the step that is completely undocumented and silently blocks the save:
   - **Release notes** (required, >0 chars)
   - **Supported Frameworks** → add **"Vaadin platform"** (then a sub-option, e.g. "Any version")
   - **Maturity** (defaults to "Experimental" — fine)
   - **License** (required) → pick e.g. **"MIT License"** from the filterable combobox
7. Click **Save** (top of the editor). The component now appears under **My components** and its
   public page is live at `/directory/component/<artifactId>` with an **Install...** menu offering
   *Maven POM*, *Download ZIP*, and *Create project*.
8. To unpublish: open the component from **My components**, untick **Published**, **Save**. It moves
   to **My unpublished components** and drops off the public directory.

## Publishing a NEW VERSION of an existing component (verified 2026-08-20, v1.1.0)

Different entry point from the first publish, and simpler — but with its own trap:

1. **My components** → click the component → **Versions** tab.
2. **Upload new version** → **Upload File...** → pick the ZIP. The dialog closes and the editor
   jumps straight to the new version's form, with **Framework, Maturity and License pre-filled
   from the previous version** (unlike the first publish, where License and Framework are empty).
   The version number comes from `Implementation-Version` in the manifest.
3. Fill **Release notes** — the only genuinely empty required field.
4. **Save**. The version appears in the table as *Published to Maven*.

**Release notes are silently truncated at 1024 characters.** No counter, no warning, no validation
error: the text is simply cut mid-word, and what you get back on the next edit is the truncated
value. Keep release notes under ~1000 characters, and re-open the version afterwards to confirm the
stored text still ends where you meant it to. (The component **Description** on the other tab holds
65000 and the **Summary** 8192, so the cap is specific to release notes.)

**A Save does not always take.** Editing the field a second time and pressing Save within the same
view silently kept the old value; navigating away, re-opening **Edit** on the version, retyping and
saving persisted it. Always verify by re-opening the editor — not by looking at the public page.

**The public component page is cached.** `/directory/component/<artifactId>` kept serving the
previous description and release notes for minutes after a successful save (a cache-busting query
parameter did not help). The editor's stored values are the authority.

## The ZIP the Directory expects (produced by `-Pdirectory`)

```
vaadoom-0.1.0.zip
├── META-INF/MANIFEST.MF        # Vaadin-Addon, Vaadin-Addon-Name, Vaadin-Addon-License, Implementation-*
├── LICENSE
├── README.md
├── vaadoom-0.1.0.jar           # the add-on
├── vaadoom-0.1.0-sources.jar
└── vaadoom-0.1.0-javadoc.jar
```
The add-on **name** and **version** shown in the editor are parsed from the manifest
(`Implementation-Title` / `Implementation-Version`). The slug/edit-URL is derived from the name
(`vaadoom`).

### What the manifest auto-fills vs. what you must still enter by hand

This asymmetry is the single most confusing part of publishing, so it is spelled out here:

| Field in the editor        | Manifest attribute            | Auto-filled from the ZIP? |
|----------------------------|-------------------------------|---------------------------|
| Component **name**         | `Implementation-Title`        | ✅ Yes — silently          |
| **Version** (creates the version row) | `Implementation-Version` | ✅ Yes — silently   |
| **Vendor / author**        | `Implementation-Vendor`       | ✅ Yes — silently          |
| **License**                | `Vaadin-Addon-License` (present!) | ❌ **No — ignored; must pick manually** |
| **Supported Frameworks**   | *(derivable from `vaadin-core` dep)* | ❌ **No — must add "Vaadin platform" manually** |
| **Release notes**          | —                             | ❌ No — manual              |
| **Summary** (>5 chars)     | —                             | ❌ No — manual              |
| **Description** (>20 chars)| —                             | ❌ No — manual              |
| **Category**               | —                             | ❌ No — manual              |

The trap: **name/version/vendor are silently fulfilled from the manifest**, so you assume the
manifest "just works" — but **License and Supported Frameworks are silently *required* and *not*
fulfilled**, even though `Vaadin-Addon-License` is right there in the manifest. Miss any required
field and **Save returns HTTP 200 with no error and silently discards the component** (it appears in
neither "My components" nor "My unpublished components").

## Gaps found in the official docs and the Vaadin MCP

The docs page *"Publish a Component → Publishing to the Vaadin Directory"* (v25.2,
`building-apps/components/publish-component-flow.md`) — which is what the Vaadin MCP
`search_vaadin_docs` returns — describes the flow as only:

> 1. Sign in. 2. Click **Publish your add-on**. 3. Upload the ZIP. 4. Fill in name, description and
> category. 5. Click **Publish**.

Every one of those five steps is inaccurate or incomplete against the live 2026 UI:

1. **No "Publish your add-on" button exists.** The entry point is **Publish component** in the
   Directory header, which goes to `/directory-edit/my-components`, where you click **New component**.
2. **No "Publish" button exists.** Publishing is a **`Published` checkbox** on the editor's
   Description tab, committed with a generic **Save**.
3. **Undocumented required fields block the save silently.** Beyond name/description/category the
   editor requires:
   - **Summary** must be **> 5 characters**; **Description** must be **> 20 characters** (doc says
     only "description").
   - **Version-level fields on a separate `Versions` tab**: **Release notes**, **Supported
     Frameworks** (must add "Vaadin platform"), and **License**. None of these are mentioned in the
     docs, and **none are auto-populated from the JAR/manifest** — notably `Vaadin-Addon-License`
     in the manifest does **not** pre-select the License field. If any are missing, **Save appears to
     succeed (HTTP 200 UIDL round-trips, no visible error) but the component is silently discarded**
     — it shows up in neither "My components" nor "My unpublished components".
4. **No Maven archetype.** The docs and MCP imply a starter but surface no archetype. There is
   **no** `mvn archetype:generate` path for v25 add-ons; the canonical scaffold is the
   **`addon-starter-flow` repo, `v25` branch** (its `master` is stale Vaadin-23/Java-11). This
   project was scaffolded by hand from that branch (see `pom.xml`).
5. **Category taxonomy is undocumented.** Top-level groups (UI/Data/Themes/Tools/Miscellaneous/
   Sponsored) expand to sub-categories (e.g. UI → *Visual Effects*, *Other UI Widgets*, …); the
   docs list none of these.

### Automation notes (Playwright)

The Directory editor is itself a Vaadin Flow app. Two things matter when driving it programmatically:

- **Setting `input.value` / Playwright `fill()` does not register with the server-side Binder** — the
  field stays `invalid` and the value is lost on Save. You must send **real keystrokes**
  (`pressSequentially`) and **blur** (Tab) so the Flow field fires its value-change/validation.
- The **License** field is a filterable, virtualized combobox: type to filter (e.g. `MIT`) then pick
  the option; the full list is not in the DOM up-front.

### Suggested doc/MCP fix

The v25.2 "Publish a Component" page should: (a) correct the entry point and the Publish-via-checkbox
mechanic; (b) enumerate the required Description-tab **and** Versions-tab fields with their
constraints; (c) state that manifest license/framework are **not** auto-applied; (d) point at
`addon-starter-flow` **v25** explicitly and note there is no archetype.
