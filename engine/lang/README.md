# The port's own text in other languages

The game's text (its menus, messages, the story) comes from the game's own data, in the languages that
release has. These catalogs translate only what the port adds:
- its Options rows, values and help lines;
- the first start's setup dialogs;
- the Vita save import.

Options > Game > Menu Language picks the language. Auto follows the game's text language. A first start,
before any setting exists, uses the system's language.

## The files

- `template.pot` lists every string. Each `<code>.po` holds one language: `ja`, `fr`, `de`, `es`, `zh-Hans`,
  `zh-Hant`, `ko`, `ru`.
- They are gettext catalogs: edit them with Poedit, Weblate or a text editor (UTF-8).
- A translation keeps the English's `{0}`, `{1}` ... in any order, and its `\n` line breaks.
- An empty `msgstr` (or one marked fuzzy) shows the English.
- Option labels and values should stay about as short as the English. A help line should fit two lines
  (about 150 Latin or 70 CJK characters).

## Trying a translation without building

Put a `<code>.po` into a `lang/` folder:
- in the game folder: beside the program, or the profile folder's;
- or in the profile folder itself.

It is read in place of the built-in one, and a new code appears in the Menu Language list. Its name comes
from the header's `X-Language-Name:` line.

## Keeping them up to date

`tools/ui_strings.py` collects the strings from the code (`tr("...")`, `trf("...", {...})`, `N_("...")`)
into `template.pot`, and brings every `.po` up to date: translations kept, new strings added untranslated,
removed ones dropped.
- `tools/ui_strings.py --new <code> "<name>"` starts a language.
- The `ui-strings` test fails while the catalogs are behind the code.

## Who translated what

- **Russian:** the Options' wording is metastigma's (CRO-U-metastigma PR #51).
- **The rest of Russian, and the other languages:** machine translations, not yet reviewed by native speakers.
  They use the game's own terms where its text has them: Gene / ジーン / 진, the Break gauge as each
  language's release names it, Pair.
- Corrections are welcome.
