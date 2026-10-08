# Name wordlists

T9 stores no dvar or menu name strings — everything is keyed by a 63-bit FNV-1a hash
(`Pointers::HashString`). Recovering a name therefore means hashing a candidate list and matching
against the hashes the live game reports. These are those candidate lists.

| File | What it is |
|---|---|
| `massive_dvar_dump.txt` | A dvar dump from a **different** Call of Duty. Its hashes use the IW prime `0x10000000233`, which appears nowhere in the BOCW image — so the **hashes are useless here and the names are not**. Wordlist only; re-hash every name through `HashString`. |
| `dvars_named.txt` | Accumulated name↔hash pairs already resolved against build `1.34.0.15931218`. Use as the seed list for `tools/dvar_hash_brute.py`. |
| `dvar_names_t9.txt` | T9-specific dvar name candidates. |
| `lui_menu_hashes.txt` | LUI menu name hashes read out of `LUI.createMenu`. Droppable unedited into `cw-mod/hashes_wanted.txt` — the in-game "Recover names from memory" scan skips any line that is not `0x…`. |

Consumers: `tools/dvar_hash_match.py`, `tools/dvar_hash_brute.py`, `tools/menu_hash_match.py`, and
the Debug tab's name-recovery scan (`Pointers::HarvestNamesFromMemory`).
