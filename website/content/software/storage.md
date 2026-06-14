---
title: "Storage of Data"
weight: 100111
---
0. Design goal

The system is a Desktop C++ CAD/CAM application for very large refinery and petrochemical complexes. Currently support Windows/DirectX12 only. In future, we will have Ubuntu/Vulkan, Android/Vulkan, Mac/Metal & iOS/Metal also. The storage model must handle:  
plant → unit → area → structure / vessel / piping set / equipment  → assemblies → parts → nut/bolt-level objects  

The system must support:  
* large object count
* multi-tab desktop editing
* server-hosted collaboration
* local .yyy virtual-server collaboration
* read-only references
* catalog files
* vendor data integration
* AutoSave
* transaction-based sync
* no per-object permission
* compact RAM layout
* stable disk layout

1. Fundamental storage principles  
RAM layout is not disk layout, Runtime memory layout and persistent disk layout are separate. The existing code already follows this direction:

META_DATA separates temporary memoryID / memoryIDParent from saved persistedId / persistedParentId. GeometryData.id uses the runtime memoryID, which is correct for rendering/selection, not durable engineering identity. Objects are allocated through the custom memory arena using a memoryGroupNo per tab. Optional64 is explicitly a compact RAM optimization: optional values are packed only when present, fixed and dynamic optionals are separated, and dynamic values use arena-managed byte references. This is excellent for runtime memory density, but it must not be persisted raw.  
  
* RAM object layout       = C++ classes + Optional64 + custom arena
* Persistent object data  = Protobuf BLOB inside SQLite
* Sync transport          = FlatBuffers messages
* GPU/render geometry     = derived runtime/cache data, not engineering truth

2. File types
.yyy file: engineering truth, .yyy file is an engineering data file. It stores:
* engineering objects
* engineering hierarchy inside the file
* object payloads
* object versions
* object lifecycle state
* object tombstones
* object-level sync metadata
* external .yyy reference alias table
* file identity
* file integrity metadata
* schema/catalog compatibility metadata
* short-term transaction/change logs
* compact permanent audit summaries

A .yyy file can be opened directly by the desktop application. A .yyy file has single access level: If a user/process has write access to the .yyy file/session, it can modify anything inside that .yyy file. No folder-level permission exists inside a .yyy. No per-object permission exists inside a .yyy. A .yyy file may reference other .yyy files as read-only references. A .yyy file can store project engineering data, reference/catalog/template data etc. No separate catalog only and data only files. It can also have reference read-only model data such as IFC, STEP etc. Some .yyy files are shipped with the application and serve as standard catalogs. All .yyy and .zzz file will have permanent / unremovable reference to these files. This is to ensure compact data representation for standardized items (ex: ISMB300).  A .yyy file may contain internal engineering containers such as: These are engineering objects, not filesystem folders.
* FOLDER
* STRUCTURE
* VESSEL
* PIPING_SET
* EQUIPMENT
* PIPE_RACK
* ASSEMBLY
* PART
* BOLT
* NUT
* PLATE
* MEMBER

A .zzz file is a project/server administrative file. A .zzz file is opened only by the server version or server-hosting mode of the application. A .zzz file does not store engineering object payloads directly. Engineering data remains in .yyy files. A .zzz file does not store engineering object payloads directly. A .zzz may mount child .zzz projects, but the project graph must remain a tree/DAG without cycles. No circular project mounting is allowed. Engineering data remains in .yyy files. A .zzz server does not require graphics rendering capability. It stores:  
* project tree
* folder hierarchy
* mounted .yyy files
* mounted child .zzz projects
* folder-level permissions
* file-level permissions
* vendor package integration
* effective permission rules
* server collaboration policy
* project-level audit metadata
* project-level schema/catalog requirements

3. Hosting and authority modes  
There are two collaboration modes.

3.1 Server-hosted .zzz mode. In this mode: central model server opens .zzz. .zzz references/mounts many .yyy files. clients connect to server. clients do not write .yyy files directly. server is the only persistent authority. Rules:
* AutoSave is enabled.
* All accepted changes are committed by the server.
* Client RAM state is transient.
* No local saved engineering copy exists.
* No child/nested peer connection from client is allowed.

That last rule is important. If client opened model through server-hosted .zzz, that client may not act as a secondary server for other peers. This prevents authority ambiguity, permission bypass, and sync loops. So:
* .zzz server → client Allowed.
* .zzz server → client → peer client Not allowed.

3.2 Local .yyy virtual-server mode. In this mode: desktop client opens local .yyy file from disk. that desktop process becomes the virtual server/host. other network peers may connect/subscribe. the host process owns persistent writes to that local .yyy. Rules:  
* AutoSave is enabled.
* Host process is the authority.
* Peers do not write the .yyy file directly.
* Peers send proposed transactions to host.
* Host validates, commits, and publishes deltas.

This gives one collaboration code path: client proposes transaction, authority commits transaction, authority publishes accepted result. The authority may be: central .zzz server OR local .yyy virtual server. But there is always exactly one persistent writer.

4. AutoSave behavior  
AutoSave is always enabled during connected editable sessions. There is no user-facing “save” operation for normal collaborative editing. A change becomes engineering-truth only after authority commit. User edits become:
* local interactive change
* proposed transaction
* server/host validation
* server/host commit
* published accepted delta
* client RAM confirmation/remap

Recommended UI states:
* CONNECTED / SAVED
* EDITING / AUTOSAVING
* COMMIT IN FLIGHT
* RECONNECTING
* STALE - RELOAD REQUIRED
* READ ONLY
* PERMISSION DENIED

5. Network interruption behavior
For server-connected clients: RAM model remains open during short network interruption. Rendering, selection, viewing, and measurement may continue. Persistent engineering commits require reconnection. When disconnected: editing continues, with local collection of change logs (non-authoritative draft transactions) in CPU RAM only. When reconnected, changes are pushed to host server/peer. Host may accept all/partial commits. Editing is blocked after 24 Hours grace period.
Since different clients may be subscribed to different sub folder, both may make independent changes to different engineering hierarchy even when disconnected, and successfully syncing latter.

6. Permission model
6.1 .yyy permission rule: A .yyy has no internal permission hierarchy. No per-object permission. No internal folder permission. Only accidental modification lock flags may exist inside .yyy. These locks are implemented in application part of the code, not database layer.  

6.2 .zzz permission rule: All real authorization belongs to .zzz / model server. Permissions may be inherited and overridden. Effective permissions are calculated by the server. Child .zzz permissions may be overridden by parent .zzz, subject to project policy. No per-object permissions. Permissions exist at: project root / folder / mounted .yyy file / mounted child .zzz

7. Object identity system
7.1 Runtime ID: Properties:
* 64-bit
* monotonically increasing in process/session
* unique across logically independent tabs
* never persisted as durable identity
* used for rendering, selection, RAM lookup, transient UI state

The code already follows this pattern by assigning memoryID at object construction and using it in geometry data.

7.2 Persistent local object ID: Persistent engineering identity inside one .yyy: local_object_id. Properties:
* stored as INTEGER / uint64_t
* only lower 40 bits used
* 0 is invalid/null
* unique inside one .yyy file
* assigned only by authority: server or local .yyy virtual-server host
* monotonic
* not reused during active project life

Do not recycle object IDs during normal operation. Reason: Recycling IDs can make old external references resolve to a wrong new object. Wrong object resolution is worse than missing/deleted resolution.  

7.3 Packed object reference References inside Protobuf payloads use compact 64-bit packed references:
* bits 63..56  reserved, currently 0
* bits 55..40  file_alias
* bits 39..0   local_object_id

Meaning: 
file_alias = 0 :same .yyy file  
file_alias = 1 :temporary same-file reference, RAM/transaction only, forbidden in committed persistent payload
file_alias = 2..65535 : external file alias, resolved through external_file table

Important distinction: object_store.object_id stores only local object ID. Protobuf references store packed file_alias + local object ID. The alias belongs to the referencing file, not the target object.

8. External file references
A .yyy may refer to other .yyy files. External file aliases are local to each .yyy. Example: In file A, catalog file C may be alias 7. In file B, the same catalog file C may be alias 12. Therefore aliases are not global. External reference resolution result must support: 
* resolved
* missing
* deleted
* moved
* replaced
* permission_denied
* file_not_loaded
* file_alias_unknown
* schema_unsupported
* corrupt_payload

9. Object lifecycle states
Use enum-style lifecycle states, not a boolean tombstone.
* 0 = live : Object exists normally.
* 1 = soft_deleted : Object is hidden from normal model view but still has payload available. Useful for: undo, short-term recovery, review
* 2 = tombstone_retained : Engineering payload may be removed or reduced. Identity and deletion metadata remain. Used to resolve external references to: deleted / replaced / moved
* 3 = purged_stub_or_archive_boundary : Allowed only after explicit compaction/archive/reference-scan policy. Does not permit ID reuse unless the entire file is rewritten as a new identity lineage.

10. Transaction, sync, and AutoSave model
THIS SECTION IS NOT STREAMLINED TO SUPPORT DIFFERENT CLIENTS EDITING DIFFERENT INDEPENDENT FOLDERS. TO BE IMPLEMENTED LATTER.
10.1 Server/host is the commit authority. Clients never directly modify authoritative SQLite files in collaboration mode. They submit proposed transactions.
Authority assigns: persistent object IDs, transaction_id, transaction_seq, change_seq, object_version, server_version

10.2 Sync ordering Use: change_seq, transaction_seq, object_version. Do not use wall-clock time for sync correctness. modified_time_utc is audit/display metadata only.

10.3 Conflict detection: Each client update should include: object_id, base_object_version, new_payload, operation_type. Authority checks: current_object_version == base_object_version.   
* If true: accept, increment object_version, assign change_seq, commit, publish delta
* If false: reject / require reload / explicit merge

Do not initially implement automatic property-level merge for engineering objects unless the object type is known safe.

10.4 Subscriptions. Clients may subscribe to: entire .yyy file / folder subtree/ object set / object type / viewport/spatial region, later

Authority publishes:
* transaction_id
* transaction_seq
* change_seq
* changed object IDs
* new object versions
* deleted/tombstone IDs
* affected parent IDs
* reference/permission errors

11. Transaction log retention
THIS SECTION IS NOT STREAMLINED TO SUPPORT DIFFERENT CLIENTS EDITING DIFFERENT INDEPENDENT FOLDERS. TO BE IMPLEMENTED LATTER.
Full transaction/change logs do not need to live forever.
Recommended defaults:
* local .yyy virtual-server mode:  object_change_log retained 30 days
* server-hosted .zzz mode: object_change_log retained 30-90 days, configurable
* object_undo_log: 7-30 days, configurable
* audit_summary: retained for project life or forever
* tombstones: retained much longer than transaction logs

If reconnect base is older than retained change_seq window:
FULL_RELOAD_REQUIRED
Use checkpointing:

object_store = current truth
sync_checkpoint = known full-state boundary
object_change_log = recent delta path
audit_summary = compact permanent trace

12. Serialization policy
12.1 Persistence: Canonical engineering object payload: Protocol Buffer BLOB

Reason:
stable schema evolution
optional fields
backward/forward compatibility
unknown field behavior
migration support
long-term persistence
12.2 Transport

Low-latency sync packets:

FlatBuffers

Used for:

object changed packets
transaction commit results
subscription updates
viewport updates
temporary-to-persistent ID remap
12.3 RAM

Runtime:

C++ object layout
Optional64
custom arena allocator
in-memory indexes
DirectX geometry cache

Optional64 is RAM-only; it supports fixed/dynamic optional properties with compact storage and generated schema/type accessors. It is not responsible for persistence and should not define the disk format.

13. SQLite naming convention
* Use: lower_snake_case Examples: object_id / parent_id / schema_version / payload_crc32c / last_writer_transaction_id
* Avoid: ALL_CAPS table names / camelCase column names / cryptic abbreviations

Use object_store, not DATA.

14. .yyy SQLite schema
Below is the proposed complete .yyy schema. This is schema specification, not implementation code.

14.1 Pragmas: Recommended when creating/opening .yyy:
* PRAGMA foreign_keys = ON;
* PRAGMA journal_mode = WAL;
* PRAGMA synchronous = NORMAL;
* PRAGMA temp_store = MEMORY;

For server-authoritative writes requiring stronger durability, use:

* PRAGMA synchronous = FULL;

14.2 file_info Stores file-level metadata.
CREATE TABLE file_info ( key   TEXT PRIMARY KEY, value BLOB NOT NULL );

Required keys:
* file_uuid : file_uuid is random immutable file identity.
* file_public_key : file_public_key is separate from file_uuid.
* file_public_key_type
* file_fingerprint
* file_format_version
* file_kind
* created_by_application
* created_time_utc
* last_saved_by_application
* last_saved_time_utc
* schema_catalog_hash
* schema_catalog_version
* object_counter_next
* last_transaction_seq
* last_change_seq
* last_server_version
* content_hash
* signature_hash
* compression_mode
* retention_policy

14.3 external_file: Maps local 16-bit file aliases to external .yyy files.

CREATE TABLE external_file (
    external_file_alias INTEGER PRIMARY KEY
        CHECK (external_file_alias BETWEEN 2 AND 65535),

    file_uuid           BLOB NOT NULL,
    file_public_key     BLOB,
    file_fingerprint    BLOB,

    canonical_uri       TEXT,
    last_known_path     TEXT,
    content_hash        BLOB,

    expected_schema_catalog_hash BLOB,

    load_policy         INTEGER NOT NULL DEFAULT 0,
    mount_state         INTEGER NOT NULL DEFAULT 0,

    permission_hint     INTEGER,
    display_name        TEXT,

    UNIQUE(file_uuid)
);

Suggested load_policy values:
0 = load_on_demand
1 = load_immediately
2 = never_auto_load
3 = required

14.4 object_store

Core engineering object table.

CREATE TABLE object_store (
    object_id       INTEGER PRIMARY KEY
        CHECK (object_id > 0 AND object_id < 1099511627776),

    parent_id       INTEGER,

    object_type     INTEGER NOT NULL,
    schema_version  INTEGER NOT NULL,
    schema_hash     BLOB,

    object_version  INTEGER NOT NULL DEFAULT 1
        CHECK (object_version > 0),

    create_seq      INTEGER NOT NULL,
    change_seq      INTEGER NOT NULL,

    server_version  INTEGER,

    lifecycle_state INTEGER NOT NULL DEFAULT 0
        CHECK (lifecycle_state BETWEEN 0 AND 3),

    lock_flags      INTEGER NOT NULL DEFAULT 0,

    created_by      BLOB,
    created_time_utc INTEGER,

    last_modified_by BLOB,
    modified_time_utc INTEGER NOT NULL,

    payload_size    INTEGER NOT NULL
        CHECK (payload_size >= 0),

    payload_crc32c  INTEGER NOT NULL,
    payload_hash    BLOB,

    last_writer_transaction_id BLOB NOT NULL,

    data            BLOB NOT NULL,

    FOREIGN KEY(parent_id)
        REFERENCES object_store(object_id)
);

Rules: object_id is local ID only, not packed reference. parent_id is same-file parent only. External parent ownership not allowed. object_version is used for conflict detection. change_seq is used for sync ordering. modified_time_utc is not sync truth. data is canonical Protobuf payload.

14.5 object_tombstone: Durable deletion/reference-resolution table.

CREATE TABLE object_tombstone (
    object_id              INTEGER PRIMARY KEY
        CHECK (object_id > 0 AND object_id < 1099511627776),

    object_type            INTEGER NOT NULL,

    deleted_seq            INTEGER NOT NULL,
    deletion_time_utc      INTEGER NOT NULL,
    deleted_by             BLOB,

    last_known_parent_id   INTEGER,
    last_known_name        TEXT,
    last_known_tag         TEXT,

    replacement_object_id  INTEGER,
    replacement_file_alias INTEGER DEFAULT 0,

    deletion_reason        INTEGER,
    last_payload_hash      BLOB,
    last_schema_hash       BLOB,

    tombstone_payload      BLOB,

    FOREIGN KEY(last_known_parent_id)
        REFERENCES object_store(object_id)
);

Rules: Tombstones are retained longer than transaction logs. Tombstone purge does not allow object ID reuse. replacement_file_alias = 0 means same file. replacement_file_alias >= 2 means external file alias.

14.6 object_relation: Non-tree engineering relationships.
THIS TABLE IS TO BE DISCARDED. THIS IS PART OF ENGINEERING DATA, NOT SUPPOSED TO BE EXPOSED TO DATABASE LAYER.
CREATE TABLE object_relation (
    relation_id       INTEGER PRIMARY KEY AUTOINCREMENT,

    source_object_id  INTEGER NOT NULL,
    target_file_alias INTEGER NOT NULL DEFAULT 0,
    target_object_id  INTEGER NOT NULL,

    relation_type     INTEGER NOT NULL,
    relation_version  INTEGER NOT NULL DEFAULT 1,

    lifecycle_state   INTEGER NOT NULL DEFAULT 0
        CHECK (lifecycle_state BETWEEN 0 AND 3),

    change_seq        INTEGER NOT NULL,
    payload_size      INTEGER NOT NULL DEFAULT 0,
    payload_crc32c    INTEGER,
    payload_hash      BLOB,
    payload           BLOB,

    FOREIGN KEY(source_object_id)
        REFERENCES object_store(object_id)
);

Use this for relationships that are not ownership hierarchy: pipe supported by support. equipment connected to nozzle. member connected to plate. instrument mounted on pipe. object references standard catalog item

14.7 transaction_log: Groups changes into engineering transactions.  
CREATE TABLE transaction_log (
    transaction_id      BLOB PRIMARY KEY,

    transaction_seq     INTEGER NOT NULL UNIQUE,

    author_id           BLOB,
    author_device_id    BLOB,
    author_session_id   BLOB,

    begin_time_utc      INTEGER,
    commit_time_utc     INTEGER NOT NULL,

    transaction_kind    INTEGER NOT NULL,

    base_server_version INTEGER,
    server_version      INTEGER,

    changed_object_count INTEGER NOT NULL DEFAULT 0,
    inserted_count       INTEGER NOT NULL DEFAULT 0,
    updated_count        INTEGER NOT NULL DEFAULT 0,
    deleted_count        INTEGER NOT NULL DEFAULT 0,
    relation_change_count INTEGER NOT NULL DEFAULT 0,

    operation_summary   TEXT,

    transaction_crc32c  INTEGER,
    transaction_hash    BLOB
);

Suggested transaction_kind:

1 = user_edit
2 = autosave_commit
3 = import
4 = vendor_merge
5 = delete
6 = replace
7 = schema_migration
8 = catalog_update
9 = server_maintenance
14.8 object_change_log

Recent sync/change metadata.

CREATE TABLE object_change_log (
    change_seq          INTEGER PRIMARY KEY AUTOINCREMENT,

    transaction_id      BLOB NOT NULL,

    object_id           INTEGER NOT NULL,
    operation_type      INTEGER NOT NULL,

    object_version      INTEGER NOT NULL,

    parent_id_before    INTEGER,
    parent_id_after     INTEGER,

    old_payload_hash    BLOB,
    new_payload_hash    BLOB,

    modified_by         BLOB,
    modified_time_utc   INTEGER NOT NULL,

    FOREIGN KEY(transaction_id)
        REFERENCES transaction_log(transaction_id)
);

Suggested operation_type:

1 = insert
2 = update
3 = move
4 = soft_delete
5 = tombstone
6 = replace
7 = purge_stub
8 = restore
14.9 relation_change_log

Recent relation sync metadata.

CREATE TABLE relation_change_log (
    relation_change_seq INTEGER PRIMARY KEY AUTOINCREMENT,

    transaction_id      BLOB NOT NULL,

    relation_id         INTEGER NOT NULL,
    operation_type      INTEGER NOT NULL,

    source_object_id    INTEGER,
    target_file_alias   INTEGER,
    target_object_id    INTEGER,
    relation_type       INTEGER,

    old_payload_hash    BLOB,
    new_payload_hash    BLOB,

    modified_by         BLOB,
    modified_time_utc   INTEGER NOT NULL,

    FOREIGN KEY(transaction_id)
        REFERENCES transaction_log(transaction_id)
);

14.10 object_undo_log: Optional short-term undo payload storage.  
CREATE TABLE object_undo_log (
    undo_id             INTEGER PRIMARY KEY AUTOINCREMENT,

    transaction_id      BLOB NOT NULL,
    object_id           INTEGER NOT NULL,

    object_version_before INTEGER,
    object_version_after  INTEGER,

    operation_type      INTEGER NOT NULL,

    old_payload_size    INTEGER,
    old_payload_crc32c  INTEGER,
    old_payload_hash    BLOB,
    old_payload         BLOB,

    new_payload_size    INTEGER,
    new_payload_crc32c  INTEGER,
    new_payload_hash    BLOB,

    created_time_utc    INTEGER NOT NULL,

    FOREIGN KEY(transaction_id)
        REFERENCES transaction_log(transaction_id)
);

Retention: 7-30 days, configurable, not required for long-term audit

14.11 audit_summary: Compact long-term audit record.  
CREATE TABLE audit_summary (
    audit_id             INTEGER PRIMARY KEY AUTOINCREMENT,

    transaction_id       BLOB NOT NULL,
    transaction_seq      INTEGER NOT NULL,

    author_id            BLOB,
    author_device_id     BLOB,

    commit_time_utc      INTEGER NOT NULL,

    changed_object_count INTEGER NOT NULL,
    inserted_count       INTEGER NOT NULL,
    updated_count        INTEGER NOT NULL,
    deleted_count        INTEGER NOT NULL,
    relation_change_count INTEGER NOT NULL DEFAULT 0,

    affected_root_id     INTEGER,
    affected_discipline  INTEGER,
    affected_file_uuid   BLOB,

    operation_summary    TEXT,

    transaction_hash     BLOB
);

Retention: project life / forever

14.12 sync_checkpoint: Defines safe boundaries for pruning logs.  
CREATE TABLE sync_checkpoint (
    checkpoint_id          INTEGER PRIMARY KEY AUTOINCREMENT,

    checkpoint_change_seq  INTEGER NOT NULL,
    checkpoint_transaction_seq INTEGER NOT NULL,

    checkpoint_time_utc    INTEGER NOT NULL,

    object_count           INTEGER NOT NULL,
    live_object_count      INTEGER NOT NULL,
    tombstone_count        INTEGER NOT NULL,
    relation_count         INTEGER NOT NULL DEFAULT 0,

    object_store_hash      BLOB,
    relation_store_hash    BLOB,
    schema_catalog_hash    BLOB,

    created_by             BLOB
);

Pruning rule:

Never prune object_change_log below a checkpoint unless current object_store
has been verified against that checkpoint policy.
14.13 schema_catalog

Stores known object schema/catalog metadata.

CREATE TABLE schema_catalog (
    schema_catalog_hash BLOB PRIMARY KEY,

    schema_catalog_version INTEGER NOT NULL,

    created_time_utc INTEGER,
    source_name      TEXT,

    descriptor_blob  BLOB,
    descriptor_hash  BLOB,

    notes            TEXT
);

14.14 object_type_registry: Maps object type IDs to schema identity.  
CREATE TABLE object_type_registry (
    object_type        INTEGER PRIMARY KEY,

    type_name          TEXT NOT NULL,
    discipline         INTEGER,
    protobuf_type_name TEXT NOT NULL,

    current_schema_version INTEGER NOT NULL,
    current_schema_hash    BLOB NOT NULL,

    flags             INTEGER NOT NULL DEFAULT 0
);

14.15 object_schema_version: Tracks schema versions per object type.  
CREATE TABLE object_schema_version (
    object_type       INTEGER NOT NULL,
    schema_version    INTEGER NOT NULL,

    schema_hash       BLOB NOT NULL,
    protobuf_type_name TEXT NOT NULL,

    migration_from_previous_available INTEGER NOT NULL DEFAULT 0,

    descriptor_blob   BLOB,
    created_time_utc  INTEGER,

    PRIMARY KEY(object_type, schema_version)
);

14.16 spatial_bounds: Optional, for viewport/region subscription and fast loading.  
CREATE TABLE spatial_bounds (
    object_id INTEGER PRIMARY KEY,

    min_x REAL NOT NULL,
    min_y REAL NOT NULL,
    min_z REAL NOT NULL,

    max_x REAL NOT NULL,
    max_y REAL NOT NULL,
    max_z REAL NOT NULL,

    bounds_version INTEGER NOT NULL,

    FOREIGN KEY(object_id)
        REFERENCES object_store(object_id)
);

Later, use SQLite R-Tree:  
CREATE VIRTUAL TABLE spatial_bounds_rtree USING rtree(
    object_id,
    min_x, max_x,
    min_y, max_y,
    min_z, max_z
);

14.17 reference_index: Optional extracted reference table for diagnostics and reference scan.  
CREATE TABLE reference_index (
    source_object_id     INTEGER NOT NULL,

    source_field_id      INTEGER,
    source_field_name    TEXT,

    target_file_alias    INTEGER NOT NULL,
    target_object_id     INTEGER NOT NULL,

    reference_kind       INTEGER NOT NULL,

    last_verified_seq    INTEGER,
    last_resolution_status INTEGER,

    PRIMARY KEY(
        source_object_id,
        source_field_id,
        target_file_alias,
        target_object_id,
        reference_kind
    ),

    FOREIGN KEY(source_object_id)
        REFERENCES object_store(object_id)
);

This table is derived from Protobuf payloads. It is not the canonical source of references.

Use it for: dangling reference scan, safe tombstone purge, impact analysis, where-used queries

14.18 integrity_issue: Persistent diagnostics table.  
CREATE TABLE integrity_issue (
    issue_id        INTEGER PRIMARY KEY AUTOINCREMENT,

    detected_time_utc INTEGER NOT NULL,
    detected_by       BLOB,

    severity        INTEGER NOT NULL,
    issue_type      INTEGER NOT NULL,

    object_id       INTEGER,
    relation_id     INTEGER,
    transaction_id  BLOB,

    message         TEXT,
    details         BLOB,

    resolved_time_utc INTEGER,
    resolved_by       BLOB,
    resolution_note   TEXT
);

14.19 .yyy indexes
CREATE INDEX idx_object_parent_live
ON object_store(parent_id, object_id)
WHERE lifecycle_state = 0;

CREATE INDEX idx_object_type_live
ON object_store(object_type, object_id)
WHERE lifecycle_state = 0;

CREATE INDEX idx_object_change_seq
ON object_store(change_seq);

CREATE INDEX idx_object_server_version
ON object_store(server_version)
WHERE server_version IS NOT NULL;

CREATE INDEX idx_object_modified_by
ON object_store(last_modified_by, change_seq);

CREATE INDEX idx_object_lifecycle
ON object_store(lifecycle_state, object_id);

CREATE INDEX idx_transaction_seq
ON transaction_log(transaction_seq);

CREATE INDEX idx_transaction_commit_time
ON transaction_log(commit_time_utc);

CREATE INDEX idx_change_log_transaction
ON object_change_log(transaction_id);

CREATE INDEX idx_change_log_object
ON object_change_log(object_id, change_seq);

CREATE INDEX idx_change_log_time
ON object_change_log(modified_time_utc);

CREATE INDEX idx_tombstone_deleted_seq
ON object_tombstone(deleted_seq);

CREATE INDEX idx_relation_source
ON object_relation(source_object_id, relation_type);

CREATE INDEX idx_relation_target
ON object_relation(target_file_alias, target_object_id, relation_type);

CREATE INDEX idx_reference_target
ON reference_index(target_file_alias, target_object_id);

CREATE INDEX idx_reference_source
ON reference_index(source_object_id);

15. .zzz SQLite schema
A .zzz file stores administrative/project truth.

15.1 .zzz project_info
CREATE TABLE project_info ( key   TEXT PRIMARY KEY, value BLOB NOT NULL);

Required keys:

project_uuid
project_public_key
project_public_key_type
project_format_version
project_name
created_by_application
created_time_utc
last_saved_time_utc
schema_catalog_hash
permission_model_version
root_folder_id
last_admin_transaction_seq
15.2 project_folder

Administrative folder tree.

CREATE TABLE project_folder (
    folder_id        INTEGER PRIMARY KEY AUTOINCREMENT,

    parent_folder_id INTEGER,

    folder_name      TEXT NOT NULL,
    folder_code      TEXT,

    display_order    INTEGER NOT NULL DEFAULT 0,

    lifecycle_state  INTEGER NOT NULL DEFAULT 0
        CHECK (lifecycle_state BETWEEN 0 AND 3),

    created_by       BLOB,
    created_time_utc INTEGER,

    modified_by      BLOB,
    modified_time_utc INTEGER,

    FOREIGN KEY(parent_folder_id)
        REFERENCES project_folder(folder_id)
);

Rules: Folders in .zzz are administrative containers. They are not engineering objects. They hold mounted .yyy files and child .zzz projects.

15.3 mounted_yyy: Files mounted into the project.  
CREATE TABLE mounted_yyy (
    mounted_file_id  INTEGER PRIMARY KEY AUTOINCREMENT,

    parent_folder_id INTEGER NOT NULL,

    file_uuid        BLOB NOT NULL,
    file_public_key  BLOB,
    file_fingerprint BLOB,

    canonical_uri    TEXT,
    last_known_path  TEXT,

    display_name     TEXT NOT NULL,
    role             INTEGER NOT NULL,

    mount_state      INTEGER NOT NULL DEFAULT 0,
    load_policy      INTEGER NOT NULL DEFAULT 0,

    schema_catalog_hash BLOB,

    permission_anchor_id INTEGER,

    created_by       BLOB,
    created_time_utc INTEGER,

    modified_by      BLOB,
    modified_time_utc INTEGER,

    UNIQUE(file_uuid),

    FOREIGN KEY(parent_folder_id)
        REFERENCES project_folder(folder_id)
);

Suggested role:

1 = engineering_model
2 = catalog
3 = vendor_model
4 = reference_model
5 = template
6 = archive
15.4 mounted_zzz

Child project/vendor package mount table.

CREATE TABLE mounted_zzz (
    mounted_project_id INTEGER PRIMARY KEY AUTOINCREMENT,

    parent_folder_id   INTEGER NOT NULL,

    child_project_uuid BLOB NOT NULL,
    child_project_public_key BLOB,
    child_project_fingerprint BLOB,

    canonical_uri      TEXT,
    last_known_path    TEXT,

    display_name       TEXT NOT NULL,

    mount_state        INTEGER NOT NULL DEFAULT 0,
    override_policy    INTEGER NOT NULL DEFAULT 0,

    permission_anchor_id INTEGER,

    created_by         BLOB,
    created_time_utc   INTEGER,

    modified_by        BLOB,
    modified_time_utc  INTEGER,

    UNIQUE(child_project_uuid),

    FOREIGN KEY(parent_folder_id)
        REFERENCES project_folder(folder_id)
);

Rules: No circular mounting. Parent .zzz may inherit or override child permissions by policy. Child .zzz remains separately identifiable.

15.5 principal: Users, groups, services.  
CREATE TABLE principal (
    principal_id      BLOB PRIMARY KEY,

    principal_type    INTEGER NOT NULL,
    display_name      TEXT NOT NULL,

    email             TEXT,
    external_auth_id  TEXT,

    lifecycle_state   INTEGER NOT NULL DEFAULT 0,

    created_time_utc  INTEGER,
    modified_time_utc INTEGER
);

Suggested principal_type:

1 = user
2 = group
3 = service_account
4 = vendor_org
5 = client_org

15.6 principal_group_member: Group membership.  
CREATE TABLE principal_group_member (
    group_principal_id  BLOB NOT NULL,
    member_principal_id BLOB NOT NULL,

    created_time_utc INTEGER,

    PRIMARY KEY(group_principal_id, member_principal_id),

    FOREIGN KEY(group_principal_id)
        REFERENCES principal(principal_id),

    FOREIGN KEY(member_principal_id)
        REFERENCES principal(principal_id)
);

15.7 permission_rule: Folder/file/project permissions.  
CREATE TABLE permission_rule (
    permission_rule_id INTEGER PRIMARY KEY AUTOINCREMENT,

    principal_id       BLOB NOT NULL,

    target_kind        INTEGER NOT NULL,
    target_id          INTEGER NOT NULL,

    permission_mask    INTEGER NOT NULL,

    inheritance_mode   INTEGER NOT NULL DEFAULT 0,
    rule_effect        INTEGER NOT NULL DEFAULT 1,

    priority           INTEGER NOT NULL DEFAULT 0,

    created_by         BLOB,
    created_time_utc   INTEGER,

    modified_by        BLOB,
    modified_time_utc  INTEGER,

    FOREIGN KEY(principal_id)
        REFERENCES principal(principal_id)
);

Suggested target_kind:

1 = project_root
2 = folder
3 = mounted_yyy
4 = mounted_zzz

Suggested rule_effect:

1 = allow
2 = deny

Suggested permission_mask bits:

1       = view
2       = edit
4       = create
8       = delete
16      = manage_permissions
32      = approve
64      = import
128     = export
256     = mount_child_project
512     = administer

No object-level target kind.

15.8 effective_permission_cache: Optional server-generated cache.  
CREATE TABLE effective_permission_cache (
    principal_id      BLOB NOT NULL,

    target_kind       INTEGER NOT NULL,
    target_id         INTEGER NOT NULL,

    effective_mask    INTEGER NOT NULL,

    computed_time_utc INTEGER NOT NULL,
    permission_version INTEGER NOT NULL,

    PRIMARY KEY(principal_id, target_kind, target_id)
);

This is disposable/recomputable.

15.9 project_admin_transaction_log: Administrative transaction log.  
CREATE TABLE project_admin_transaction_log (
    admin_transaction_id BLOB PRIMARY KEY,

    admin_transaction_seq INTEGER NOT NULL UNIQUE,

    author_id          BLOB,
    commit_time_utc    INTEGER NOT NULL,

    operation_kind     INTEGER NOT NULL,
    operation_summary  TEXT,

    transaction_hash   BLOB
);

Used for: folder changes, file mounting, permission changes, vendor package mounting, project metadata changes

15.10 project_audit_summary
CREATE TABLE project_audit_summary (
    audit_id              INTEGER PRIMARY KEY AUTOINCREMENT,

    admin_transaction_id  BLOB NOT NULL,
    admin_transaction_seq INTEGER NOT NULL,

    author_id             BLOB,
    commit_time_utc       INTEGER NOT NULL,

    operation_kind        INTEGER NOT NULL,
    operation_summary     TEXT,

    transaction_hash      BLOB
);

15.11 .zzz indexes
CREATE INDEX idx_project_folder_parent
ON project_folder(parent_folder_id, display_order);

CREATE INDEX idx_mounted_yyy_folder
ON mounted_yyy(parent_folder_id, display_name);

CREATE INDEX idx_mounted_yyy_uuid
ON mounted_yyy(file_uuid);

CREATE INDEX idx_mounted_zzz_folder
ON mounted_zzz(parent_folder_id, display_name);

CREATE INDEX idx_mounted_zzz_uuid
ON mounted_zzz(child_project_uuid);

CREATE INDEX idx_permission_principal
ON permission_rule(principal_id);

CREATE INDEX idx_permission_target
ON permission_rule(target_kind, target_id);

CREATE INDEX idx_effective_permission_principal
ON effective_permission_cache(principal_id);

CREATE INDEX idx_admin_transaction_seq
ON project_admin_transaction_log(admin_transaction_seq);

16. Integrity fields
16.1 payload_crc32c: Fast accidental corruption detection.  
Used for: BLOB truncation, wrong decompression, bad read, memory corruption before write, partial payload write, Not a security mechanism.

16.2 payload_size: Stores expected serialized Protobuf byte size.  
Used for: preallocation, truncation detection, sanity checking, payload limits, 

16.3 schema_hash: Identifies persistence schema, not RAM layout.  
Should represent: object type, protobuf message identity, field numbers, field types, semantic constraints, migration version, unit metadata if schema-bound

16.4 last_writer_transaction_id: Links each object to the transaction that last wrote it.  
Used for: audit, debugging, rollback tooling, sync diagnosis

16.5 file_uuid: Immutable random file identity.  
Used for: external reference resolution, file move/rename detection, vendor package identity, duplicate detection, catalog identity. Do not overload it as an ED448 public key.  
Use: file_uuid, file_public_key, file_fingerprint as separate fields.

16.6 schema_catalog_hash: Whole catalog/schema compatibility fingerprint.
Used to detect whether the executable understands the file’s object types and persistence schemas.

17. Object reference resolution
Given packed reference: reserved_bits, file_alias, object_id  
Resolution process:  
* 1. reserved_bits must be 0.
* 2. object_id = 0 means null reference.
* 3. file_alias = 0 means current .yyy.
* 4. file_alias = 1 is temporary; invalid in committed storage.
* 5. file_alias >= 2 resolves through external_file.
* 6. Check whether target file is loaded.
* 7. Check permission if server-hosted.
* 8. Check object_store.
* 9. Check object_tombstone.
* 10. Return resolution status.

Possible status:

resolved
null_reference
temporary_reference
file_alias_unknown
file_not_loaded
permission_denied
missing
deleted
moved
replaced
schema_unsupported
corrupt_payload

18. Server/client restriction summary
18.1 Server-hosted .zzz
Allowed: 
* server opens .zzz
* server opens/mounts .yyy files
* client connects to server
* client edits through AutoSave transactions
* server persists
* server publishes deltas

Forbidden:  
* client saving local authoritative copy
* client acting as child server
* peer connecting through client
* client writing .yyy directly
* client bypassing .zzz permissions

18.2 Local .yyy virtual server
Allowed:
* desktop opens local .yyy
* desktop becomes authority
* network peers connect to it
* AutoSave commits to local .yyy through host
* host publishes deltas

Forbidden:  
* peers writing .yyy file directly
* multiple independent writers to same .yyy
* per-object permission
* nested server chain beyond local host policy

19. Current finalized rules
These are the strongest finalized rules so far:  
* .yyy is engineering truth.
* .zzz is administrative/project truth.
* Protobuf is persistent object payload.
* FlatBuffers is sync/transport.
* Optional64 is RAM-only.
* AutoSave is always enabled in editable connected sessions.
* Server-hosted .zzz clients cannot host child peer sessions.
* Local .yyy opened from disk may act as virtual server.
* No per-object permission.
* No folder permission inside .yyy.
* Permissions live in .zzz only.
* Persistent object IDs are server/host assigned.
* Client temporary IDs are RAM/transaction-only.
* Object IDs are not recycled during active project life.
* object_store.object_id is local object ID only.
* Packed references include file_alias + local object_id.
* modified_time_utc is audit/display only. Not for sync.

FOLLOWING NEED FURTHER DELIBERATION:  
* change_seq is sync truth.
* object_version is conflict truth.
* Tombstones live longer than transaction logs.
* Full transaction logs may be pruned after checkpoints.
* Audit summary remains long-term.

20. One recommendation before implementation
Before coding the SQLite layer, freeze these enum registries in one design document:

object_type
relation_type
operation_type
transaction_kind
lifecycle_state
reference_resolution_status
permission_mask bits
mounted file role
load_policy
mount_state

That enum registry is as important as the table schema. It will become the common language between:

C++ runtime
Protobuf schema
FlatBuffers sync packets
SQLite storage
server validation
debug tools
future migration tools
