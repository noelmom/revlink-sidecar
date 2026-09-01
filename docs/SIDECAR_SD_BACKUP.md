# Sidecar microSD backup and recovery

RevLink stores device namespaces, immutable cached objects, manifests,
annotations, startup-image profiles, and system metadata on microSD. The
customer portal provides a portable logical backup of user datasets. A
physical card copy remains the complete development/disaster-recovery image.

## Browser backup and restore

Open **Settings → Backup & restore** while the microSD is healthy and no
AccessPort file session or synchronization is active.

- **Download backup** streams a versioned `.revlink-backup` archive containing
  every cached device dataset, including maps, datalogs, screenshots, startup
  image profiles, manifests, history, and notes.
- **Choose backup** uploads an archive to a temporary file and validates its
  format, path allowlist, file count, sizes, and SHA-256 digest for every file
  before offering a restore.
- **Restore missing data** is a non-destructive merge. Missing files are
  written through a temporary file and verified before rename. Identical files
  are skipped. Existing paths with different contents are reported as
  conflicts and are never overwritten.

The logical archive intentionally excludes Sidecar identity, hardware MAC,
Wi-Fi credentials, onboarding acceptance, firmware, update payloads, and
transient recovery files. Store downloaded backups somewhere separate from the
Sidecar.

## Required backup checkpoints

Create a full backup:

- after a meaningful new-device or large incremental sync;
- before changing the on-card schema or migration code;
- before testing missing, unreadable, format, or recovery behavior;
- before any destructive storage acceptance; and
- before replacing or reformatting the development card.

A routine UI-only firmware flash does not modify the card, but a backup is
required before flashing firmware that changes storage or recovery code.

## Create a verified backup

1. Finish or cancel active synchronization.
2. Hold BOOT for two seconds and wait for the safe-shutdown screen.
3. Remove power, remove the card, and mount it on the development Mac.
4. Locate the mounted volume under `/Volumes`.
5. Run:

   ```sh
   scripts/backup_sidecar_sd.sh /Volumes/<SD-volume>
   ```

Backups are written outside the repository by default:

```text
/Volumes/Projects/Backups/RevLink-Sidecar-SD/
├── revlink-sidecar-sd-<UTC>.tar.gz
└── revlink-sidecar-sd-<UTC>.tar.gz.sha256
```

The archive contains:

- `card/` — the complete copied filesystem;
- `SHA256SUMS` — a digest for every copied file; and
- `BACKUP_INFO.txt` — creation time, source label, counts, and source commit.

The tool creates the archive through a `.partial` file, checks that the
archive can be read, writes its SHA-256, and verifies that checksum before
reporting success.

## Inspect and verify

Verify the archive itself:

```sh
cd /Volumes/Projects/Backups/RevLink-Sidecar-SD
shasum -a 256 -c revlink-sidecar-sd-<UTC>.tar.gz.sha256
```

Verify every backed-up file without touching an SD card:

```sh
work_dir="$(mktemp -d)"
tar -xzf revlink-sidecar-sd-<UTC>.tar.gz -C "${work_dir}"
(
  cd "${work_dir}/card"
  shasum -a 256 -c ../SHA256SUMS
)
```

Remove the temporary directory afterward.

## Physical restore policy

Physical restore is destructive and is never automatic. Use a known-good, correctly
formatted replacement card, verify the archive checksum first, and copy the
contents of `card/` to the empty card. Reinsert it only after the copy is
complete and the host has safely ejected the volume.

Do not restore over a card containing the only copy of newer data. Preserve
that card separately, and compare manifests before deciding which version is
authoritative. Never use the Sidecar's guarded format workflow merely to
prepare a restore target unless data loss has been explicitly approved.
