// Copyright (c) 2026 by the XRootD Collaboration. LGPL-3.0-or-later.
#include "XrdAcc/XrdAccAuthorize.hh"
#include "XrdOfs/XrdOfsPrepPersist.hh"
#include "XrdOfs/XrdOfsPrepStore.hh"
#include "XrdOuc/XrdOucBuffer.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucErrInfo.hh"
#include "XrdOuc/XrdOucTList.hh"
#include "XrdSec/XrdSecEntity.hh"
#include "XrdSec/XrdSecEntityAttr.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace XrdOfsPrepProtocol;
namespace fs = std::filesystem;
namespace {
struct Temporary {
  std::string path;
  Temporary() { char name[] = "/tmp/xrd-prep-test-XXXXXX"; auto *p = mkdtemp(name); if (!p) throw std::runtime_error("mkdtemp"); path = p; }
  ~Temporary() { std::error_code ec; fs::remove_all(path, ec); }
};
struct Args {
  XrdSfsPrep prep{};
  std::string id;
  XrdOucTListFIFO paths, opaque;
  Args(const std::string &request, int opts, std::initializer_list<std::string> files = {}, const std::string &cgi = "") : id(request) {
    int i = 0;
    for (const auto &path : files) { paths.Add(new XrdOucTList(path.c_str(), i++)); opaque.Add(new XrdOucTList(cgi.c_str())); }
    if (files.size() == 0 && !cgi.empty()) opaque.Add(new XrdOucTList(cgi.c_str()));
    prep.reqid = &id[0]; prep.opts = opts; prep.paths = paths.first; prep.oinfo = opaque.first;
  }
};
std::string Text(XrdOucErrInfo &error) {
  std::string text(error.getErrText(), error.getErrInfo());
  while (!text.empty() && !text.back()) text.pop_back();
  return text;
}
int Reply(XrdOucErrInfo &error, const Json &json) {
  auto text = json.dump(); void *memory = nullptr;
  if (posix_memalign(&memory, sizeof(void *), text.size() + 1)) throw std::bad_alloc();
  std::memcpy(memory, text.c_str(), text.size() + 1);
  error.setErrInfo(text.size() + 1, new XrdOucBuffer(static_cast<char *>(memory), text.size() + 1));
  return SFS_DATA;
}
bool Eventually(const std::function<bool()> &predicate) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  do { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
  while (std::chrono::steady_clock::now() < end);
  return false;
}
class Backend : public XrdOfsPrepare {
public:
  std::mutex mutex;
  Json records = Json::object();
  bool hold = false, unavailable = false, failAfterAccept = false;
  unsigned stages = 0;
  int begin(XrdSfsPrep &args, XrdOucErrInfo &error, const XrdSecEntity *) override { return Apply(args, error); }
  int cancel(XrdSfsPrep &args, XrdOucErrInfo &error, const XrdSecEntity *) override { return Apply(args, error); }
  int query(XrdSfsPrep &args, XrdOucErrInfo &error, const XrdSecEntity *) override {
    std::lock_guard<std::mutex> guard(mutex);
    if (unavailable) { error.setErrInfo(EIO, "offline"); return SFS_ERROR; }
    Json response = {{"schema", 1}, {"requestId", args.reqid}, {"known", false}, {"acknowledged", Json::array()}, {"files", Json::array()}};
    auto it = records.find(args.reqid);
    if (it != records.end()) {
      response = *it;
      if (!hold) for (auto &file : response["files"]) if (file["state"] == "STARTED") file["state"] = "COMPLETED";
    }
    return Reply(error, response);
  }
  int Apply(XrdSfsPrep &args, XrdOucErrInfo &error) {
    std::lock_guard<std::mutex> guard(mutex);
    XrdOucEnv env(args.oinfo ? args.oinfo->text : nullptr);
    const std::string op = env.Get("xrd.prepare.operation") ? env.Get("xrd.prepare.operation") : "";
    if (op.empty() || env.Get("authz")) { error.setErrInfo(EINVAL, "unsafe operation"); return SFS_ERROR; }
    auto &record = records[args.reqid];
    if (record.is_null()) record = {{"schema", 1}, {"requestId", args.reqid}, {"known", true}, {"acknowledged", Json::array()}, {"files", Json::array()}};
    for (const auto &ack : record["acknowledged"]) if (ack == op) return SFS_OK;
    if (args.opts & Prep_STAGE) {
      ++stages;
      for (auto *path = args.paths; path; path = path->next) record["files"].push_back({{"path", path->text}, {"state", hold ? "STARTED" : "COMPLETED"}});
    } else if (args.opts & Prep_CANCEL) {
      for (auto *path = args.paths; path; path = path->next)
        for (auto &file : record["files"]) if (file["path"] == path->text && !Terminal(file["state"])) file["state"] = "CANCELLED";
    }
    record["acknowledged"].push_back(op);
    if (failAfterAccept) { error.setErrInfo(EIO, "lost reply"); return SFS_ERROR; }
    return SFS_OK;
  }
};
std::string Submit(XrdOfsPrepPersist &coordinator, XrdSecEntity &owner, const std::string &cgi = "") {
  std::string id;
  if (!Eventually([&] {
    Args args("*", Prep_STAGE, {"/a", "/b"}, cgi); XrdOucErrInfo error("test");
    const auto rc = coordinator.begin(args.prep, error, &owner);
    if (rc == SFS_DATA) { id = Text(error); return true; }
    if (error.getErrInfo() != EAGAIN) throw std::runtime_error(error.getErrText());
    return false;
  })) throw std::runtime_error("admission timed out");
  return id;
}
Json Query(XrdOfsPrepPersist &coordinator, const std::string &id, XrdSecEntity &owner) {
  Args args(id, Prep_QUERY); XrdOucErrInfo error("test");
  if (coordinator.query(args.prep, error, &owner) != SFS_DATA) throw std::runtime_error(error.getErrText());
  return Json::parse(Text(error));
}
}

TEST(PrepStore, AtomicRevisionShardingAndSingleWriter) {
  Temporary directory;
  std::string id = XrdOfsPrepStore::NewId();
  {
    XrdOfsPrepStore store(directory.path + "/state");
    EXPECT_THROW(XrdOfsPrepStore second(directory.path + "/state"), std::system_error);
    Json record = {{"schema", 1}, {"id", id}, {"value", "before"}};
    store.Save(record, true);
    EXPECT_EQ(store.Load(id)["revision"], 1);
    EXPECT_TRUE(fs::exists(store.Root() / "requests" / id.substr(0, 2) / id.substr(2, 2) / (id + ".json")));
    record["value"] = "after"; store.Save(record);
    EXPECT_EQ(store.Load(id)["revision"], 2);
    EXPECT_THROW(store.Save(record, true), std::system_error);
  }
  XrdOfsPrepStore reopened(directory.path + "/state");
  EXPECT_EQ(reopened.Load(id)["value"], "after");
}
TEST(PrepStore, RejectsCorruptionFutureVersionAndSymlinks) {
  Temporary directory; XrdOfsPrepStore store(directory.path);
  const auto id = store.NewId(); Json record = {{"schema", 1}, {"id", id}}; store.Save(record, true);
  auto path = store.Root() / "requests" / id.substr(0, 2) / id.substr(2, 2) / (id + ".json");
  std::ofstream(path) << "broken";
  EXPECT_THROW(store.Load(id), std::system_error);
  record["schema"] = 2; store.Save(record);
  EXPECT_THROW(store.Load(id), std::system_error);
  fs::remove(path); fs::create_symlink("/etc/passwd", path);
  EXPECT_THROW(store.Load(id), std::system_error);
  EXPECT_THROW(store.Load("../../etc/passwd"), std::system_error);
}
TEST(PrepProtocol, MetadataAndPaths) {
  EXPECT_EQ(Path("//a///b"), "/a/b");
  EXPECT_THROW(Path("/a/../b"), std::system_error);
  EXPECT_THROW(Path("/a\n/b"), std::system_error);
  EXPECT_EQ(Metadata({{"diskLifetime", "PT1H"}})["diskLifetime"], "PT1H");
  EXPECT_THROW(Metadata({{"diskLifetime", 5}}), std::system_error);
  EXPECT_EQ(Unhex(Hex("heterogeneous metadata")), "heterogeneous metadata");
}
TEST(PrepPersist, AdmissionOwnershipSubsetAndTombstone) {
  Temporary directory; Backend backend; backend.hold = true;
  XrdSecEntity alice("unix"), bob("unix"); alice.name = const_cast<char *>("alice"); bob.name = const_cast<char *>("bob");
  XrdOfsPrepPersist coordinator(directory.path, "test", backend, nullptr, nullptr);
  auto id = Submit(coordinator, alice, "authz=not-persisted");
  EXPECT_TRUE(IsId(id));
  ASSERT_TRUE(Eventually([&] { return Query(coordinator, id, alice)["files"][0]["state"] == "STARTED"; }));
  Args query(id, Prep_QUERY); XrdOucErrInfo error("test");
  EXPECT_EQ(coordinator.query(query.prep, error, &bob), SFS_ERROR);
  EXPECT_EQ(error.getErrInfo(), EACCES);
  Args invalid(id, Prep_CANCEL, {"/a", "/missing"});
  EXPECT_EQ(coordinator.cancel(invalid.prep, error, &alice), SFS_ERROR);
  EXPECT_EQ(error.getErrInfo(), EINVAL);
  EXPECT_EQ(Query(coordinator, id, alice)["files"][0]["state"], "STARTED");
  Args cancel(id, Prep_CANCEL, {"/a"});
  EXPECT_EQ(coordinator.cancel(cancel.prep, error, &alice), SFS_OK);
  ASSERT_TRUE(Eventually([&] { return Query(coordinator, id, alice)["files"][0]["state"] == "CANCELLED"; }));
  EXPECT_EQ(Query(coordinator, id, alice)["files"][1]["state"], "STARTED");
  Args erase(std::string(DeletePrefix) + id, Prep_CANCEL);
  EXPECT_EQ(coordinator.cancel(erase.prep, error, &alice), SFS_OK);
  EXPECT_EQ(coordinator.query(query.prep, error, &alice), SFS_ERROR);
  EXPECT_EQ(error.getErrInfo(), ENOENT);
  auto path = fs::path(directory.path) / "requests" / id.substr(0, 2) / id.substr(2, 2) / (id + ".json");
  std::ifstream stream(path);
  std::stringstream ss;
  ss << stream.rdbuf();
  std::string bytes = ss.str();
  EXPECT_EQ(bytes.find("not-persisted"), std::string::npos);
  EXPECT_TRUE(Json::parse(bytes)["deleted"].get<bool>());
}
TEST(PrepPersist, RestartAfterBackendAcceptedButReplyWasLost) {
  Temporary directory; Backend backend; backend.failAfterAccept = true;
  XrdSecEntity alice("unix"); alice.name = const_cast<char *>("alice");
  std::string id;
  {
    XrdOfsPrepPersist coordinator(directory.path, "test", backend, nullptr, nullptr);
    id = Submit(coordinator, alice);
    ASSERT_TRUE(Eventually([&] { std::lock_guard<std::mutex> lock(backend.mutex); return backend.stages == 1; }));
  }
  {
    XrdOfsPrepPersist restarted(directory.path, "test", backend, nullptr, nullptr);
    ASSERT_TRUE(Eventually([&] { return Query(restarted, id, alice)["files"][0]["state"] == "COMPLETED"; }));
  }
  EXPECT_EQ(backend.stages, 1u);
}
TEST(PrepPersist, BackendOutageDoesNotLoseAcceptedIntent) {
  Temporary directory; Backend backend; backend.unavailable = true;
  XrdSecEntity alice("unix"); alice.name = const_cast<char *>("alice"); std::string id;
  {
    XrdOfsPrepPersist coordinator(directory.path, "test", backend, nullptr, nullptr);
    id = Submit(coordinator, alice);
    EXPECT_EQ(Query(coordinator, id, alice)["files"][0]["state"], "SUBMITTED");
  }
  backend.unavailable = false;
  XrdOfsPrepPersist restarted(directory.path, "test", backend, nullptr, nullptr);
  ASSERT_TRUE(Eventually([&] { return Query(restarted, id, alice)["files"][0]["state"] == "COMPLETED"; }));
}
TEST(PrepStore, SubdirSyncOnExistingDirectory) {
  Temporary directory;
  XrdOfsPrepStore store(directory.path + "/state");
  std::string id = XrdOfsPrepStore::NewId();
  const std::string s1 = id.substr(0, 2);
  const std::string s2 = id.substr(2, 2);
  fs::create_directories(store.Root() / "requests" / s1 / s2);
  Json record = {{"schema", 1}, {"id", id}, {"value", "initial"}};
  EXPECT_NO_THROW(store.Save(record, true));
  EXPECT_EQ(store.Load(id)["value"], "initial");
}
class RejectingBackend : public Backend {
public:
  int begin(XrdSfsPrep &, XrdOucErrInfo &error, const XrdSecEntity *) override {
    error.setErrInfo(ENOTSUP, "operation not supported");
    return SFS_ERROR;
  }
};
TEST(PrepPersist, PermanentBackendRejectionFailsRequest) {
  Temporary directory; RejectingBackend backend;
  XrdSecEntity alice("unix"); alice.name = const_cast<char *>("alice");
  XrdOfsPrepPersist coordinator(directory.path, "test", backend, nullptr, nullptr);
  auto id = Submit(coordinator, alice);
  ASSERT_TRUE(Eventually([&] {
    auto status = Query(coordinator, id, alice);
    return status["files"][0]["state"] == "FAILED";
  }));
  auto status = Query(coordinator, id, alice);
  EXPECT_EQ(status["files"][0]["state"], "FAILED");
  EXPECT_EQ(status["files"][1]["state"], "FAILED");
  EXPECT_EQ(status["files"][0]["error"], "operation not supported");
  EXPECT_TRUE(status.contains("completedAt"));
}
class MockAuthorizer : public XrdAccAuthorize {
public:
  XrdAccPrivs Access(const XrdSecEntity *entity, const char *, Access_Operation,
                     XrdOucEnv *env) override {
    const char *token = env ? env->Get("authz") : nullptr;
    if (token) {
      std::string sub = token;
      for (const char *prefix : {"Bearer%20", "Bearer "})
        if (sub.compare(0, std::strlen(prefix), prefix) == 0) sub.erase(0, std::strlen(prefix));
      if (sub == "alice" || sub == "bob") {
        entity->eaAPI->Add("token.subject", sub, true);
        entity->eaAPI->Add("token.issuer", "test-issuer", true);
        return XrdAccPriv_All;
      }
    }
    return XrdAccPriv_None;
  }
  int Audit(int, const XrdSecEntity *, const char *, Access_Operation, XrdOucEnv *) override { return 1; }
  int Test(XrdAccPrivs privileges, Access_Operation) override { return privileges != XrdAccPriv_None; }
};
TEST(PrepPersist, NativeCgiAuthenticationPreserved) {
  Temporary directory; Backend backend; backend.hold = true;
  MockAuthorizer authorizer;
  XrdOucEnv env;
  env.PutPtr("XrdAccAuthorize*", &authorizer);
  XrdSecEntity client("unix"); client.name = const_cast<char *>("alice");
  XrdOfsPrepPersist coordinator(directory.path, "test", backend, &env, nullptr);

  std::string id = Submit(coordinator, client, "authz=alice");
  EXPECT_TRUE(IsId(id));

  // Native query without paths: args.paths is null, args.oinfo carries "authz=alice"
  Args queryAlice(id, Prep_QUERY, {}, "authz=alice");
  XrdOucErrInfo error("test");
  ASSERT_TRUE(Eventually([&] {
    if (coordinator.query(queryAlice.prep, error, &client) != SFS_DATA) return false;
    auto qres = Json::parse(Text(error));
    return qres["files"].size() == 2u && qres["files"][0]["state"] == "STARTED";
  }));

  // Query with wrong token is denied
  Args queryBob(id, Prep_QUERY, {}, "authz=bob");
  EXPECT_EQ(coordinator.query(queryBob.prep, error, &client), SFS_ERROR);
  EXPECT_EQ(error.getErrInfo(), EACCES);

  // Query without any token is denied
  Args queryAnon(id, Prep_QUERY);
  EXPECT_EQ(coordinator.query(queryAnon.prep, error, &client), SFS_ERROR);
  EXPECT_EQ(error.getErrInfo(), EACCES);

  // Subset cancel on /b only with correct CGI
  Args cancelB(id, Prep_CANCEL, {"/b"}, "authz=alice");
  EXPECT_EQ(coordinator.cancel(cancelB.prep, error, &client), SFS_OK);
  ASSERT_TRUE(Eventually([&] {
    return coordinator.query(queryAlice.prep, error, &client) == SFS_DATA &&
           Json::parse(Text(error))["files"][1]["state"] == "CANCELLED";
  }));

  // Subset cancel on /a with wrong CGI is denied
  Args cancelAWrong(id, Prep_CANCEL, {"/a"}, "authz=bob");
  EXPECT_EQ(coordinator.cancel(cancelAWrong.prep, error, &client), SFS_ERROR);
  EXPECT_EQ(error.getErrInfo(), EACCES);
}

