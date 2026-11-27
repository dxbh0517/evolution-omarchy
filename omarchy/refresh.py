#!/usr/bin/env python3
"""Inspect mail polling and push settings; apply changes only when requested."""
import argparse
from pathlib import Path
from datetime import datetime, timezone
import gi

gi.require_version('EDataServer', '1.2')
from gi.repository import EDataServer

LOCAL_BACKENDS = ('maildir', 'mbox', 'vfolder', 'none', 'rss')

# Candidate EDS extension names + GObject property for server push.
PUSH_BY_BACKEND = {
    'imapx': (('Imapx Backend',), 'use-idle'),
    'ews': (('EWS Settings', 'Ews Backend', 'EWS Backend'), 'listen-notifications'),
    'microsoft365': (
        ('Microsoft365 Settings', 'Microsoft 365 Settings', 'Microsoft365 Backend', 'M365 Settings'),
        'listen-notifications',
    ),
}


def mail_accounts(registry):
    return [
        source for source in registry.list_sources('Mail Account')
        if registry.check_enabled(source)
        and source.get_extension('Mail Account').get_backend_name() not in LOCAL_BACKENDS
    ]


def _push_targets(source):
    """Yield (holder, prop) objects that expose the push setting."""
    backend = source.get_extension('Mail Account').get_backend_name()
    spec = PUSH_BY_BACKEND.get(backend)
    if not spec:
        return
    extension_names, prop = spec
    for extension_name in extension_names:
        extension = source.get_extension(extension_name)
        if extension is None:
            continue
        yield extension, prop
        settings = None
        if hasattr(extension, 'get_settings'):
            try:
                settings = extension.get_settings()
            except Exception:
                settings = None
        if settings is not None:
            yield settings, prop


def push_state(source):
    """Return (label, enabled_or_None) for this account's push setting."""
    backend = source.get_extension('Mail Account').get_backend_name()
    spec = PUSH_BY_BACKEND.get(backend)
    if not spec:
        return None, None
    _, prop = spec
    for holder, holder_prop in _push_targets(source):
        try:
            return holder_prop, bool(holder.get_property(holder_prop))
        except Exception:
            continue
    return prop, None


def ensure_push(source):
    """Enable IMAP IDLE / EWS listen-notifications. Returns True if changed."""
    changed = False
    for holder, prop in _push_targets(source):
        try:
            if holder.get_property(prop):
                continue
            holder.set_property(prop, True)
            changed = True
        except Exception:
            continue
    return changed


def status_line(index, source, refresh):
    backend = source.get_extension('Mail Account').get_backend_name()
    parts = [
        f'Account {index} ({source.get_display_name()}, {backend}):',
        f'polling {"on" if refresh.get_enabled() else "off"}',
        f'{refresh.get_interval_minutes()} minute(s)',
    ]
    prop, enabled = push_state(source)
    if prop:
        if enabled is True:
            parts.append(f'{prop}=on')
        elif enabled is False:
            parts.append(f'{prop}=off')
        else:
            parts.append(f'{prop}=unknown')
    return ' '.join(parts)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    '--apply-minutes',
    type=int,
    metavar='N',
    help='set enabled mail accounts to check every N minutes (1–60)',
)
parser.add_argument(
    '--ensure-realtime',
    action='store_true',
    help=(
        'enable IMAP IDLE / EWS listen-notifications and a gentle refresh '
        'fallback (10 minutes when push is on, 1 minute when it is not). '
        'A 1-minute poll on every folder fights IDLE and causes retrieve failures.'
    ),
)
args = parser.parse_args()

if args.apply_minutes is not None and not 1 <= args.apply_minutes <= 60:
    parser.error('interval must be between 1 and 60 minutes')

apply_minutes = args.apply_minutes
# Default fallback for --ensure-realtime is chosen per-account below once
# we know whether push (IDLE / listen) is available.

registry = EDataServer.SourceRegistry.new_sync(None)
accounts = mail_accounts(registry)
backup = None

for index, source in enumerate(accounts, 1):
    refresh = source.get_extension('Refresh')
    notes = []

    if apply_minutes is not None or args.ensure_realtime:
        if not source.get_writable():
            print(f'Account {index}: read-only; skipped')
            continue
        if backup is None:
            stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
            backup = (
                Path.home()
                / '.local/share/evolution-omarchy/backups'
                / ('refresh-' + stamp)
            )
            backup.mkdir(parents=True, mode=0o700)

        data, _ = source.to_string()
        path = backup / f'account-{index}.source'
        path.touch(mode=0o600)
        path.write_text(data)

        if args.ensure_realtime:
            if ensure_push(source):
                notes.append('push enabled')
            else:
                prop, enabled = push_state(source)
                if enabled is True:
                    notes.append('push already on')
                elif prop:
                    notes.append(f'could not set {prop}')

            # Prefer inbox-focused polling when IDLE is carrying real-time.
            backend = source.get_extension('Mail Account').get_backend_name()
            if backend == 'imapx':
                imapx = source.get_extension('Imapx Backend')
                if imapx is not None:
                    try:
                        if imapx.get_property('check-all'):
                            imapx.set_property('check-all', False)
                            notes.append('check-all=off')
                    except Exception:
                        pass

        minutes = apply_minutes
        if minutes is None and args.ensure_realtime:
            prop, enabled = push_state(source)
            # IDLE/listen already delivers new mail; poll only as a safety net.
            minutes = 10 if enabled else 1

        if minutes is not None:
            refresh.set_enabled(True)
            refresh.set_interval_minutes(minutes)
            notes.append(f'interval={minutes}m')

        source.write_sync(None)

    line = status_line(index, source, refresh)
    if notes:
        line += ' [' + ', '.join(notes) + ']'
    print(line)

if not accounts:
    print('No enabled remote mail accounts found.')
if backup:
    print(
        'Previous account settings backed up privately under '
        '~/.local/share/evolution-omarchy/backups/.'
    )
