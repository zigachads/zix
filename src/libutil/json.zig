const std = @import("std");

pub fn parse_from_file(
    path: [*:0]const u8,
) callconv(.C) ?*std.json.Value {
    const file = std.fs.openFileAbsoluteZ(path, .{}) catch |err| {
        std.log.err(
            "json_parse_from_file(\"{s}\") failed {}",
            .{ path[0..std.mem.len(path)], err },
        );
        return null;
    };
    defer file.close();

    var json_reader = std.json.reader(std.heap.c_allocator, file.reader());
    const json = std.json.parseFromTokenSource(
        std.json.Value,
        std.heap.c_allocator,
        &json_reader,
        .{ .allocate = .alloc_if_needed },
    ) catch |err| {
        std.log.err("json_parse_from_file(\"{s}\") failed {}", .{
            path[0..std.mem.len(path)],
            err,
        });
        return null;
    };
    defer json.deinit();

    const self = std.heap.c_allocator.create(std.json.Value) catch @panic("OOM");

    self.* = json.value;

    return self;
}

pub fn object_get(
    json_value: *std.json.Value,
    key: [*:0]const u8,
) callconv(.C) ?*std.json.Value {
    const value = json_value.object.get(key[0..std.mem.len(key)]);

    if (value) |result_value| {
        const result = std.heap.c_allocator.create(std.json.Value) catch @panic("OOM");

        result.* = result_value;

        return result;
    } else return null;
}

pub fn object_set(
    json_value: *std.json.Value,
    key: [*:0]const u8,
    to_insert: *std.json.Value,
) callconv(.C) bool {
    json_value.object.put(key[0..std.mem.len(key)], to_insert.*) catch return false;

    return true;
}

pub fn object_set_integer(
    json_value: *std.json.Value,
    key: [*:0]const u8,
    to_insert: i64,
) callconv(.C) bool {
    json_value.object.put(key[0..std.mem.len(key)], std.json.Value{
        .integer = to_insert,
    }) catch return false;

    return true;
}

pub fn object_set_bool(
    json_value: *std.json.Value,
    key: [*:0]const u8,
    to_insert: bool,
) callconv(.C) bool {
    json_value.object.put(key[0..std.mem.len(key)], std.json.Value{
        .bool = to_insert,
    }) catch return false;

    return true;
}

pub fn object_update(
    self: *std.json.Value,
    other: *std.json.Value,
) callconv(.C) bool {
    var iterator = other.object.iterator();

    while (iterator.next()) |entry| {
        self.object.put(entry.key_ptr.*, entry.value_ptr.*) catch return false;
    }

    return true;
}

pub fn object_set_string(
    json_value: *std.json.Value,
    key: [*:0]const u8,
    to_insert: [*:0]const u8,
) callconv(.C) bool {
    json_value.object.put(key[0..std.mem.len(key)], std.json.Value{
        .string = to_insert[0..std.mem.len(to_insert)],
    }) catch return false;

    return true;
}

pub fn object_set_strings(
    json_value: *std.json.Value,
    key: [*:0]const u8,
    strings: [*][*:0]const u8,
    size: usize,
) callconv(.C) bool {
    for (0..size) |i| {
        const to_insert = strings[i];

        json_value.object.put(key[0..std.mem.len(key)], std.json.Value{
            .string = to_insert[0..std.mem.len(to_insert)],
        }) catch return false;
    }

    return true;
}

pub fn new_object() callconv(.C) *std.json.Value {
    const self = std.heap.c_allocator.create(std.json.Value) catch @panic("OOM");

    self.* = std.json.Value{
        .object = std.json.ObjectMap.init(std.heap.c_allocator),
    };

    return self;
}

pub fn new_string(string_value: [*:0]const u8) callconv(.C) *std.json.Value {
    const self = std.heap.c_allocator.create(std.json.Value) catch @panic("OOM");

    self.* = std.json.Value{
        .string = string_value[0..std.mem.len(string_value)],
    };

    return self;
}

pub fn string_get(
    json_value: *std.json.Value,
) callconv(.C) [*:0]const u8 {
    return std.heap.c_allocator.dupeZ(u8, json_value.string) catch @panic("OOM");
}

pub fn new_integer(integer_value: i64) callconv(.C) *std.json.Value {
    const self = std.heap.c_allocator.create(std.json.Value) catch @panic("OOM");

    self.* = std.json.Value{
        .integer = integer_value,
    };

    return self;
}

pub fn new_list() callconv(.C) *std.json.Value {
    const self = std.heap.c_allocator.create(std.json.Value) catch @panic("OOM");

    self.* = std.json.Value{
        .array = std.json.Array.init(std.heap.c_allocator),
    };

    return self;
}

pub fn list_insert(json_value: *std.json.Value, to_insert: *std.json.Value) callconv(.C) void {
    json_value.array.append(to_insert.*) catch {
        @panic("Failed to insert a value into a list");
    };
}

pub fn free(json_value: *std.json.Value) callconv(.C) void {
    switch (json_value.*) {
        .object => |*object| {
            object.deinit();
        },
        .array => |*array| {
            array.deinit();
        },
        else => {},
    }

    std.heap.c_allocator.destroy(json_value);
}

comptime {
    @export(&parse_from_file, .{ .name = "nix_libutil_json_parse_from_file" });
    @export(&new_object, .{ .name = "nix_libutil_json_object_new" });
    @export(&object_get, .{ .name = "nix_libutil_json_object_get" });
    @export(&object_set, .{ .name = "nix_libutil_json_object_set" });
    @export(&object_set_integer, .{ .name = "nix_libutil_json_object_set_integer" });
    @export(&object_set_string, .{ .name = "nix_libutil_json_object_set_string" });
    @export(&object_set_bool, .{ .name = "nix_libutil_json_object_set_bool" });
    @export(&object_set_strings, .{ .name = "nix_libutil_json_object_set_strings" });
    @export(&object_update, .{ .name = "nix_libutil_json_object_update" });
    @export(&new_string, .{ .name = "nix_libutil_json_string_new" });
    @export(&string_get, .{ .name = "nix_libutil_json_string_get" });
    @export(&new_integer, .{ .name = "nix_libutil_json_integer_new" });
    @export(&new_list, .{ .name = "nix_libutil_json_list_new" });
    @export(&list_insert, .{ .name = "nix_libutil_json_list_insert" });
    @export(&free, .{ .name = "nix_libutil_json_free" });
}
