#!/usr/bin/env python3
"""Compare exported management JSON profiles without applying or filling defaults."""
import argparse
import json
import math
from pathlib import Path

from host_paths import external_output


def reject_constant(value):
    raise ValueError('non-finite JSON number: ' + value)


def load_profile(path):
    if path.stat().st_size > 49152:
        raise ValueError('profile exceeds the document 41 source budget')
    value = json.loads(path.read_text(encoding='utf-8'), parse_constant=reject_constant)
    if not isinstance(value, dict):
        raise ValueError('profile must be a JSON object')
    profile = value.get('profile', value)
    if not isinstance(profile, dict) or not isinstance(profile.get('profile_id'), str) or not profile['profile_id']:
        raise ValueError('profile_id is required; ROS1/YAML values have no implicit mapping')
    return value.get('revision') if 'profile' in value else None, profile


def compare_profiles(previous, current, revision=None):
    if previous['profile_id'] != current['profile_id']:
        raise ValueError('profile identities differ; select the mapping explicitly before comparison')
    patches, missing, invalid = [], [], []
    for field in ('startup_timeout', 'video_required'):
        if field not in previous or field not in current:
            missing.append(field)
            continue
        value = previous[field]
        valid = (type(value) in (int, float) and 1 <= value <= 180 and math.isfinite(value)) if field == 'startup_timeout' else type(value) is bool
        if field == 'video_required' and value is True and current.get('video') is not True:
            valid = False
        if not valid:
            invalid.append(field)
        elif type(value) is not type(current[field]) or value != current[field]:
            # Numeric int/float encodings have identical protocol meaning.
            if field != 'startup_timeout' or value != current[field]:
                patches.append(dict(profile_id=current['profile_id'], field=field, value=value))
    unmapped = sorted(field for field in previous.keys() | current.keys()
                      if field not in ('startup_timeout', 'video_required', 'config_revision')
                      and (field not in previous or field not in current or previous[field] != current[field]))
    return dict(format_version=1, scope='management_policy_comparison_only', profile_id=current['profile_id'],
                base_revision=revision, proposed_patches=patches, missing_fields=missing, invalid_fields=invalid,
                unmapped_differences=unmapped, requires_review=True, applied=False,
                device_nvm_written=False, physical_acceptance=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--previous', required=True, type=Path)
    parser.add_argument('--current', required=True, type=Path, help='exported current config.source result')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        output = external_output(args.output)
        _, previous = load_profile(args.previous)
        revision, current = load_profile(args.current)
        report = compare_profiles(previous, current, revision)
        # Exclusive creation keeps previous evidence intact.
        with output.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2); stream.write('\n')
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(json.dumps(dict(output=str(output), applied=False, requires_review=True)))
    return 1 if report['missing_fields'] or report['invalid_fields'] or report['unmapped_differences'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
