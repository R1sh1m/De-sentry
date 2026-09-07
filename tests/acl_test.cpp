// Per-collection ACL test suite.
//
// The rule the whole model rests on: a collection is either shared with the
// mesh or private to an owner plus an explicit reader list. Enforcement has to
// hold at three places, and all three are checked here:
//
//   1. The catalog's own predicates (`CanRead` / `CanWrite`).
//   2. The engine's CRUD path, where a refused read must be indistinguishable
//      from a missing collection.
//   3. Inheritance through `parent`, including what happens when the metadata
//      is malformed -- an ACL that fails *open* on a cycle would be a way to
//      read anything by hand-editing one file.
//
// Plain assert(), no framework (CMakeLists.txt keeps assertions live with
// -UNDEBUG in every build type).

#include <algorithm>
#include <cassert>
#include <iostream>
#include <memory>
#include <string>

#include "desentry/common/platform.h"
#include "desentry/engine/node_engine.h"
#include "desentry/storage/catalog.h"

using namespace desentry;

namespace {

const char* kOwner = "node-owner";
const char* kFriend = "node-friend";
const char* kStranger = "node-stranger";

std::string TestRoot() { return AppDataDir() + "/desentry_test_acl"; }

void Fresh(const std::string& path) {
  RemoveTree(path);
  MakeDirs(path);
}

JsonValue Doc(const std::string& value) {
  JsonValue::Object fields;
  fields.emplace_back("value", JsonValue(value));
  return JsonValue(std::move(fields));
}

CollectionAcl PrivateTo(const std::string& owner, std::vector<std::string> readers = {}) {
  CollectionAcl acl;
  acl.owner_node = owner;
  acl.is_private = true;
  acl.readers = std::move(readers);
  return acl;
}

// -- catalog-level predicates -----------------------------------------------

void TestCatalogPredicates() {
  std::cout << "  catalog: read and write predicates\n";
  const std::string dir = TestRoot() + "/catalog";
  Fresh(dir);

  auto catalog_or = Catalog::Open(dir + "/catalog.json");
  assert(catalog_or.ok());
  auto& catalog = *catalog_or.value();

  assert(catalog.CreateCollection("open", 1).ok());
  assert(catalog.CreateCollection("secret", 2).ok());
  assert(catalog.SetAcl("secret", PrivateTo(kOwner, {kFriend})).ok());

  // A collection with no ACL is shared: anyone on the mesh may read it and
  // anyone may write to it. That is the CRDT default, and making it implicit
  // rather than requiring an ACL on every collection is deliberate.
  assert(catalog.CanRead("open", kStranger));
  assert(catalog.CanWrite("open", kStranger));

  // A private collection admits its owner and its named readers, nobody else.
  assert(catalog.CanRead("secret", kOwner));
  assert(catalog.CanRead("secret", kFriend));
  assert(!catalog.CanRead("secret", kStranger));

  // Writes to a private collection are the owner's alone. A reader who could
  // write would be able to change what the owner sees, which is not what
  // "shared with" means.
  assert(catalog.CanWrite("secret", kOwner));
  assert(!catalog.CanWrite("secret", kFriend));
  assert(!catalog.CanWrite("secret", kStranger));

  // An unknown collection reads as shared: it does not exist yet, and the
  // first write creates it. Refusing here would make every first write fail.
  assert(catalog.CanRead("never-created", kStranger));

  RemoveTree(dir);
}

void TestAclInheritance() {
  std::cout << "  catalog: inheritance through parent\n";
  const std::string dir = TestRoot() + "/inherit";
  Fresh(dir);

  auto catalog_or = Catalog::Open(dir + "/catalog.json");
  assert(catalog_or.ok());
  auto& catalog = *catalog_or.value();

  assert(catalog.CreateCollection("project", 1).ok());
  assert(catalog.CreateCollection("project.notes", 2).ok());
  assert(catalog.SetAcl("project", PrivateTo(kOwner, {kFriend})).ok());

  CollectionAcl child;
  child.parent = "project";
  assert(catalog.SetAcl("project.notes", child).ok());

  // The child has no readers of its own; it gets the parent's.
  const CollectionAcl effective = catalog.EffectiveAcl("project.notes");
  assert(effective.is_private);
  assert(effective.owner_node == kOwner);
  assert(catalog.CanRead("project.notes", kFriend));
  assert(!catalog.CanRead("project.notes", kStranger));

  RemoveTree(dir);
}

void TestAclCycleFailsClosed() {
  std::cout << "  catalog: a cycle denies rather than allows\n";
  const std::string dir = TestRoot() + "/cycle";
  Fresh(dir);

  auto catalog_or = Catalog::Open(dir + "/catalog.json");
  assert(catalog_or.ok());
  auto& catalog = *catalog_or.value();

  assert(catalog.CreateCollection("a", 1).ok());
  assert(catalog.CreateCollection("b", 2).ok());

  CollectionAcl to_b;
  to_b.parent = "b";
  to_b.is_private = true;
  to_b.owner_node = kOwner;
  CollectionAcl to_a;
  to_a.parent = "a";
  to_a.is_private = true;
  to_a.owner_node = kOwner;
  assert(catalog.SetAcl("a", to_b).ok());
  assert(catalog.SetAcl("b", to_a).ok());

  // Resolution must terminate, and must land on "deny" rather than "allow".
  // A cycle is malformed metadata; treating malformed metadata as permission
  // would turn hand-editing one file into a way to read everything.
  assert(!catalog.CanRead("a", kStranger));
  assert(!catalog.CanRead("b", kStranger));
  assert(!catalog.CanRead("a", kFriend));

  RemoveTree(dir);
}

// -- engine-level enforcement -----------------------------------------------

void TestEngineEnforcement() {
  std::cout << "  engine: CRUD refuses on ACL\n";
  const std::string dir = TestRoot() + "/engine";
  Fresh(dir);

  NodeEngine::Options options;
  options.data_dir = dir;
  options.engines = {"kv"};
  options.default_engine = "kv";
  auto engine_or = NodeEngine::Open(options);
  assert(engine_or.ok());
  auto& engine = *engine_or.value();

  const std::string self = engine.identity().node_id();
  const Requestor local = engine.SelfRequestor();
  const Requestor friendly = Requestor::Peer(kFriend);
  const Requestor stranger = Requestor::Peer(kStranger);

  // A shared collection: everyone reads, everyone writes. This is replication
  // working, not a hole.
  assert(engine.PutDocument("shared", "k1", Doc("hello"), local).ok());
  assert(engine.GetDocument("shared", "k1", stranger).ok());
  assert(engine.PutDocument("shared", "k2", Doc("from a peer"), stranger).ok());

  // Now make one private to this node, with one named reader.
  assert(engine.PutDocument("journal", "j1", Doc("private thought"), local).ok());
  assert(engine.storage().catalog().SetAcl("journal", PrivateTo(self, {kFriend})).ok());

  assert(engine.GetDocument("journal", "j1", local).ok());
  assert(engine.GetDocument("journal", "j1", friendly).ok());

  // A stranger's read fails, and fails as NotFound rather than as a permission
  // error: an error that distinguishes "you may not read this" from "this does
  // not exist" tells the stranger the collection is there.
  auto refused = engine.GetDocument("journal", "j1", stranger);
  assert(!refused.ok());
  assert(refused.status().code() == StatusCode::kNotFound);

  // A named reader may not write.
  assert(!engine.PutDocument("journal", "j2", Doc("nope"), friendly).ok());
  assert(!engine.DeleteDocument("journal", "j1", friendly).ok());
  assert(!engine.PutDocument("journal", "j2", Doc("nope"), stranger).ok());

  // The owner still can.
  assert(engine.PutDocument("journal", "j2", Doc("mine"), local).ok());
  assert(engine.DeleteDocument("journal", "j2", local).ok());

  // A listing must not leak keys either -- refusing the document but
  // returning its key would still say what exists.
  auto listed = engine.ListDocuments("journal", "", 100, stranger);
  assert(listed.empty());
  auto listed_by_reader = engine.ListDocuments("journal", "", 100, friendly);
  assert(!listed_by_reader.empty());

  // ...and neither must the collection index.
  auto visible = engine.ReadableCollections(stranger);
  assert(std::find(visible.begin(), visible.end(), std::string("journal")) == visible.end());
  assert(std::find(visible.begin(), visible.end(), std::string("shared")) != visible.end());

  auto owner_view = engine.ReadableCollections(local);
  assert(std::find(owner_view.begin(), owner_view.end(), std::string("journal")) != owner_view.end());

  // The transit store is internal bookkeeping and is never listed as a
  // collection, whoever is asking.
  for (const std::string& name : engine.ReadableCollections(local)) {
    assert(name != kTransitCollection);
  }

  RemoveTree(dir);
}

void TestAclSurvivesReopen() {
  std::cout << "  engine: an ACL survives a restart\n";
  const std::string dir = TestRoot() + "/persist";
  Fresh(dir);

  NodeEngine::Options options;
  options.data_dir = dir;
  options.engines = {"kv"};
  options.default_engine = "kv";

  std::string self;
  {
    auto engine_or = NodeEngine::Open(options);
    assert(engine_or.ok());
    auto& engine = *engine_or.value();
    self = engine.identity().node_id();
    assert(engine.PutDocument("journal", "j1", Doc("private"), engine.SelfRequestor()).ok());
    assert(engine.storage().catalog().SetAcl("journal", PrivateTo(self, {kFriend})).ok());
  }

  {
    // An ACL that only lived in memory would silently open every private
    // collection on the next restart -- the failure nobody would notice.
    auto engine_or = NodeEngine::Open(options);
    assert(engine_or.ok());
    auto& engine = *engine_or.value();
    assert(!engine.GetDocument("journal", "j1", Requestor::Peer(kStranger)).ok());
    assert(engine.GetDocument("journal", "j1", Requestor::Peer(kFriend)).ok());
    assert(engine.identity().node_id() == self);
  }

  RemoveTree(dir);
}

}  // namespace

int main() {
  std::cout << "== ACL test suite ==\n";
  Fresh(TestRoot());

  TestCatalogPredicates();
  TestAclInheritance();
  TestAclCycleFailsClosed();
  TestEngineEnforcement();
  TestAclSurvivesReopen();

  RemoveTree(TestRoot());
  std::cout << "acl_test: ALL PASSED\n";
  return 0;
}
