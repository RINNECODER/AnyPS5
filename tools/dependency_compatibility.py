#!/usr/bin/env python3
"""Static qualified dependency inventory; never reads or executes guest binaries."""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile

CLASSIFICATIONS = ('supplied_guest', 'qualified_host', 'missing_provider', 'mismatch', 'unknown')
SCOPE_FIELDS = ('nid', 'library', 'library_version', 'module', 'module_major', 'module_minor')
SYMBOL_FIELDS = SCOPE_FIELDS + ('type', 'size', 'binding', 'visibility', 'section')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def bare_filename(value):
    return isinstance(value, str) and value not in ('', '.', '..') and not any(c in value for c in '/\\:\r\n')


def load_json(path):
    path = Path(path).resolve(strict=True)
    data = path.read_bytes()
    result = json.loads(data)
    require(isinstance(result, dict), f'{path}: expected JSON object')
    return result, {'metadata_path': str(path), 'metadata_sha256': hashlib.sha256(data).hexdigest()}


def module_key(item):
    if 'name' in item:
        version = item['version']
        return item['name'], version >> 8, version & 255
    return item['module'], item['module_major'], item['module_minor']


def scope(item):
    return tuple(item[field] for field in SCOPE_FIELDS)


def valid_scope(item):
    return (all(isinstance(item.get(key), str) and item[key] for key in ('nid', 'library', 'module'))
            and all(integer(item.get(key)) for key in ('library_version', 'module_major', 'module_minor'))
            and item['module_major'] <= 255 and item['module_minor'] <= 255)


def validate_metadata(data, label):
    require(isinstance(data.get('path'), str) and bare_filename(Path(data['path']).name), f'{label}: missing source path')
    require(integer(data.get('size')) and re.fullmatch('[0-9a-f]{64}', data.get('sha256', '')) is not None,
            f'{label}: missing source size/sha256')
    for key in ('needed_files', 'import_modules', 'import_libraries', 'export_modules', 'export_libraries', 'imports', 'exports'):
        require(isinstance(data.get(key), list), f'{label}: missing {key} array')
    require(all(bare_filename(name) for name in data['needed_files']), f'{label}: invalid DT_NEEDED filename')
    for key in ('import_modules', 'import_libraries', 'export_modules', 'export_libraries'):
        for item in data[key]:
            require(isinstance(item, dict) and isinstance(item.get('name'), str) and item['name']
                    and integer(item.get('version')) and item['version'] <= 65535, f'{label}: invalid {key} identity')
    for direction in ('imports', 'exports'):
        modules = {module_key(item) for item in data[direction[:-1] + '_modules']}
        libraries = {(item['name'], item['version']) for item in data[direction[:-1] + '_libraries']}
        for item in data[direction]:
            require(isinstance(item, dict) and valid_scope(item), f'{label}: incomplete {direction} identity')
            require(all(integer(item.get(field)) for field in ('type', 'size', 'binding', 'visibility', 'section')),
                    f'{label}: incomplete {direction} symbol attributes')
            require(module_key(item) in modules and (item['library'], item['library_version']) in libraries,
                    f'{label}: {direction} qualifiers lack declared module/library')
            require(item['visibility'] <= 3, f'{label}: invalid visibility')
            require(item['binding'] > 0 and (item['section'] == 0 if direction == 'imports' else item['section'] > 0),
                    f'{label}: invalid {direction} binding/section')


def validate_host(data):
    require(data.get('schema_version') == 1, 'Host evidence schema_version must be 1')
    require(isinstance(data.get('source'), dict)
            and re.fullmatch('[0-9a-f]{40}', data['source'].get('commit', '')) is not None,
            'Host evidence must record frozen source commit')
    require(isinstance(data.get('modules'), list) and isinstance(data.get('exports'), list),
            'Host evidence requires modules and exports arrays')
    for item in data['modules']:
        require(isinstance(item, dict) and bare_filename(item.get('filename'))
                and isinstance(item.get('module'), str) and item['module']
                and integer(item.get('module_major')) and integer(item.get('module_minor')),
                'Incomplete host module declaration')
        require(isinstance(item.get('libraries'), list), 'Host declaration requires explicit libraries')
        for library in item['libraries']:
            require(isinstance(library, dict) and isinstance(library.get('library', library.get('name')), str)
                    and library.get('library', library.get('name'))
                    and integer(library.get('library_version', library.get('version'))), 'Incomplete host library declaration')
    for item in data['exports']:
        require(isinstance(item, dict) and valid_scope(item), 'Incomplete host export identity')
        require(item.get('qualification') in ('qualified', 'unknown'), 'Host export qualification must be qualified or unknown')
        if item['qualification'] == 'qualified':
            require(integer(item.get('type')) and integer(item.get('size')) and item.get('evidence'),
                    'Qualified host export requires type, size and resolver evidence')


def declares(host, symbol):
    return (module_key(host) == module_key(symbol)
            and any((lib.get('library', lib.get('name')), lib.get('library_version', lib.get('version'))) == (symbol['library'], symbol['library_version']) for lib in host['libraries']))


def provider_ref(provider, symbol=None):
    result = {'filename': provider['filename'], 'kind': provider['kind']}
    if symbol is not None:
        result['symbol'] = {key: symbol[key] for key in SYMBOL_FIELDS if key in symbol}
        if 'evidence' in symbol:
            result['evidence'] = symbol['evidence']
    return result


def build_report(consumer_metadata_paths, host_evidence_path):
    """First metadata is main; remaining metadata is the complete supplied guest set."""
    require(consumer_metadata_paths, 'At least one consumer metadata path is required')
    consumers, identities = [], []
    for path in consumer_metadata_paths:
        data, identity = load_json(path)
        validate_metadata(data, str(path))
        data = dict(data, filename=Path(data['path']).name, kind='guest')
        consumers.append(data)
        identities.append(dict(identity, filename=data['filename'], source_sha256=data['sha256'], source_size=data['size']))
    filenames = [item['filename'] for item in consumers]
    require(len(set(filenames)) == len(filenames), 'Duplicate supplied guest filenames')
    host, host_identity = load_json(host_evidence_path)
    validate_host(host)
    policy = host.get('effective_host_policy', {})
    hosts = [dict(item, kind='host') for item in host['modules']]
    suppressed = []
    if policy.get('suppress_matching_guest_modules') is True:
        guest_modules = {module_key(item) for consumer in consumers[1:] for item in consumer['export_modules']}
        suppressed = [item for item in hosts if module_key(item) in guest_modules]
        hosts = [item for item in hosts if module_key(item) not in guest_modules]
    graph_issues = []
    needed_all = {name for consumer in consumers for name in consumer['needed_files']}
    if policy.get('kernel_filename_alias') is True:
        for item in hosts:
            if item['module'] == 'libkernel':
                if {'libkernel.prx', 'libkernel.sprx'} <= needed_all:
                    graph_issues.append({'classification': 'mismatch', 'reason': 'both_kernel_filename_aliases_requested'})
                elif 'libkernel.prx' in needed_all:
                    item['filename'] = 'libkernel.prx'
    if not policy:
        graph_issues.append({'classification': 'unknown', 'reason': 'effective_host_selection_policy_not_recorded'})
    host_names = [item['filename'] for item in hosts]
    if len(set(host_names)) != len(host_names) or set(filenames) & set(host_names):
        graph_issues.append({'classification': 'mismatch', 'reason': 'invalid_or_ambiguous_host_filename_declaration'})
    guest_by_scope, guest_by_nid = defaultdict(list), defaultdict(list)
    for consumer in consumers:
        for symbol in consumer['exports']:
            if symbol['visibility'] not in (1, 2):
                guest_by_scope[scope(symbol)].append((consumer, symbol))
                guest_by_nid[symbol['nid']].append((consumer, symbol))
    host_by_scope, host_by_nid = defaultdict(list), defaultdict(list)
    for symbol in host['exports']:
        if symbol.get('active') is False:
            continue
        host_by_scope[scope(symbol)].append(symbol)
        host_by_nid[symbol['nid']].append(symbol)
    output_consumers, closures = [], defaultdict(set)
    totals = Counter()
    for consumer in consumers:
        result = {'filename': consumer['filename'], 'source_sha256': consumer['sha256'],
                  'needed_files': [], 'import_modules': [], 'imports': []}
        for name in consumer['needed_files']:
            guests = [item for item in consumers if item['filename'] == name]
            declared_hosts = [item for item in hosts if item['filename'] == name]
            if guests and declared_hosts:
                category, reason = 'mismatch', 'ambiguous_guest_host_filename'
            elif guests:
                category, reason = 'supplied_guest', 'file_metadata_supplied_only'
            elif declared_hosts:
                category, reason = 'unknown', 'host_filename_declared_symbol_support_is_separate'
            else:
                category, reason = 'missing_provider', 'no_supplied_guest_or_declared_host_filename'
                closures[name].add(consumer['filename'])
            result['needed_files'].append({'filename': name, 'classification': category, 'reason': reason})
        for needed in consumer['import_modules']:
            key = module_key(needed)
            guests = [item for item in consumers if any(module_key(export) == key for export in item['export_modules'])]
            declared_hosts = [item for item in hosts if module_key(item) == key]
            count = len(guests) + len(declared_hosts)
            category = 'mismatch' if count > 1 else ('supplied_guest' if guests else 'unknown' if declared_hosts else 'missing_provider')
            result['import_modules'].append({'module': key[0], 'module_major': key[1], 'module_minor': key[2],
                'provider_count': count, 'classification': category,
                'providers': [provider_ref(item) for item in guests + declared_hosts]})
        for symbol in consumer['imports']:
            row = {key: symbol[key] for key in SYMBOL_FIELDS}
            if 'symbol_index' in symbol:
                row['symbol_index'] = symbol['symbol_index']
            candidates = guest_by_scope[scope(symbol)]
            declared_hosts = [item for item in hosts if declares(item, symbol)]
            module_count = sum(any(module_key(item) == module_key(symbol) for item in guest['export_modules']) for guest in consumers)
            module_count += sum(module_key(item) == module_key(symbol) for item in hosts)
            row['providers'] = [provider_ref(item, export) for item, export in candidates]
            category, reason = 'unknown', 'unclassified'
            if symbol['type'] not in (1, 2, 6):
                category, reason = 'mismatch', 'unsupported_import_type'
            elif module_count > 1:
                category, reason = 'mismatch', 'ambiguous_named_module_version_provider'
            elif any(export['type'] != symbol['type'] for _, export in candidates):
                category, reason = 'mismatch', 'guest_symbol_type_mismatch'
            elif len(candidates) > 1:
                category, reason = 'mismatch', 'ambiguous_guest_export_provider'
            elif candidates and declared_hosts:
                category, reason = 'mismatch', 'ambiguous_guest_host_scope_provider'
            elif candidates:
                _, export = candidates[0]
                if export['section'] >= 0xff00:
                    category, reason = 'mismatch', 'unsupported_absolute_guest_export'
                elif symbol['size'] > export['size']:
                    category, reason = 'mismatch', 'guest_import_exceeds_export_storage'
                elif symbol['type'] in (1, 6):
                    category, reason = 'unknown', 'guest_object_storage_or_tls_backing_not_proved'
                else:
                    category, reason = 'supplied_guest', 'exact_guest_scope_type_and_symbol_attributes_match'
            elif len(declared_hosts) > 1:
                category, reason = 'mismatch', 'ambiguous_host_scope_provider'
            elif declared_hosts:
                exports = host_by_scope[scope(symbol)]
                if len(exports) > 1 and all(integer(item.get('resolver_order')) for item in exports):
                    first = min(item['resolver_order'] for item in exports)
                    exports = [item for item in exports if item['resolver_order'] == first]
                row['providers'] = [provider_ref(declared_hosts[0], export) for export in exports]
                if symbol['type'] == 6:
                    category, reason = 'mismatch', 'host_tls_imports_unsupported'
                elif not exports:
                    category, reason = ('missing_provider', 'complete_cpu_resolver_inventory_has_no_nid') if host.get('resolver_inventory_complete') is True else ('unknown', 'declared_host_scope_has_no_qualified_nid_evidence')
                elif len(exports) > 1:
                    category, reason = 'mismatch', 'ambiguous_host_export_evidence'
                elif integer(exports[0].get('type')) and exports[0]['type'] != symbol['type']:
                    category, reason = 'mismatch', 'host_symbol_type_mismatch'
                elif 'active' in exports[0] and exports[0]['active'] is None:
                    category, reason = 'unknown', 'host_resolver_activation_not_proved'
                elif exports[0]['qualification'] != 'qualified':
                    category, reason = 'unknown', 'host_abi_contract_not_qualified'
                elif symbol['type'] == 1 and (not exports[0]['size'] or symbol['size'] > exports[0]['size']):
                    category, reason = 'mismatch', 'host_object_storage_size_mismatch'
                else:
                    category, reason = 'qualified_host', 'exact_cpu_host_scope_nid_type_evidence'
            else:
                near = guest_by_nid[symbol['nid']] + [(item, export) for item in hosts for export in host_by_nid[symbol['nid']] if declares(item, export)]
                if near:
                    category, reason = 'mismatch', 'nid_available_only_with_different_scope_or_version'
                    row['providers'] = [provider_ref(item, export) for item, export in near]
                else:
                    category, reason = 'missing_provider', 'no_exact_guest_export_or_declared_cpu_host_scope'
            if module_count == 0 and category in ('supplied_guest', 'qualified_host'):
                category, reason = 'mismatch', 'missing_named_module_version_provider'
            row.update(classification=category, reason=reason)
            totals[category] += 1
            result['imports'].append(row)
        result['summary'] = dict(Counter(row['classification'] for row in result['imports']))
        output_consumers.append(result)
    return {'schema_version': 1, 'report_kind': 'static_qualified_dependency_compatibility',
        'tool': {'path': str(Path(__file__).resolve()), 'sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()},
        'source': host['source'], 'inputs': {'consumers': identities, 'host_evidence': host_identity},
        'classification_basis': 'Frozen CPU resolver identity/type rules; supplied_guest and qualified_host describe static evidence only.',
        'runtime_compatibility': 'not_established',
        'limits': ['No guest binaries read or executed.', 'Guest profile, mappings, CRT, relocations and service semantics are not proved.',
                   'Guest object readable storage and TLS backing are not available in these metadata inputs.',
                   'Missing-provider dependency closure cannot be inferred from the consumers.'],
        'resolver_inventory_complete': host.get('resolver_inventory_complete') is True,
        'effective_host_policy': policy, 'effective_host_modules': hosts, 'suppressed_host_modules': suppressed,
        'graph_issues': graph_issues, 'host_unknowns': host.get('unknowns', []),
        'summary': {key: totals[key] for key in CLASSIFICATIONS}, 'consumers': output_consumers,
        'unknown_provider_closure': [{'filename': name, 'required_by': sorted(required), 'classification': 'unknown',
                                      'reason': 'absent_provider_metadata_closure_unknown'} for name, required in sorted(closures.items())]}


def write_report(report, output_path, input_paths):
    output = Path(output_path).absolute()
    require(output.parent.exists(), 'Report output parent must already exist')
    output = output.parent.resolve() / output.name
    require(not output.is_relative_to(Path(__file__).resolve().parents[1]), 'Report output must be outside the tool Git worktree')
    source_repository = report.get('source', {}).get('repository')
    if source_repository:
        require(not output.is_relative_to(Path(source_repository).resolve()), 'Report output must be outside the frozen source repository')
    require(output not in {Path(path).resolve() for path in input_paths}, 'Report must not overwrite input metadata')
    require(not output.exists() and not output.is_symlink(), 'Report output already exists; choose a new explicit path')
    data = (json.dumps(report, indent=2, sort_keys=True) + '\n').encode()
    descriptor, temporary = tempfile.mkstemp(prefix='.dependency-report-', dir=output.parent)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        # Link publishes atomically without replacing a concurrently created output.
        os.link(temporary, output)
    finally:
        os.unlink(temporary)
    return hashlib.sha256(data).hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--consumer-metadata', action='append', required=True,
                        help='JSON metadata: first main, then every supplied guest dependency')
    parser.add_argument('--host-evidence', required=True, help='Frozen qualified CPU resolver evidence JSON')
    parser.add_argument('--output', required=True, help='New private JSON report path; existing outputs are rejected')
    args = parser.parse_args(argv)
    try:
        report = build_report(args.consumer_metadata, args.host_evidence)
        checksum = write_report(report, args.output, args.consumer_metadata + [args.host_evidence])
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(2, f'dependency compatibility: {error}\n')
    print(json.dumps({'output': str(Path(args.output).absolute()), 'sha256': checksum, 'summary': report['summary']}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
