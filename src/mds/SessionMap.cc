// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "MDS.h"
#include "MDCache.h"
#include "Mutation.h"
#include "SessionMap.h"
#include "osdc/Filer.h"

#include "common/config.h"
#include "common/errno.h"
#include "include/assert.h"
#include "include/stringify.h"

#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix *_dout << "mds." << rank << ".sessionmap "


class SessionMapIOContext : public MDSIOContextBase
{
  protected:
    SessionMap *sessionmap;
    MDS *get_mds() {return sessionmap->mds;}
  public:
    SessionMapIOContext(SessionMap *sessionmap_) : sessionmap(sessionmap_) {
      assert(sessionmap != NULL);
    }
};



void SessionMap::dump()
{
  dout(10) << "dump" << dendl;
  for (ceph::unordered_map<entity_name_t,Session*>::iterator p = session_map.begin();
       p != session_map.end();
       ++p) 
    dout(10) << p->first << " " << p->second
	     << " state " << p->second->get_state_name()
	     << " completed " << p->second->info.completed_requests
	     << " prealloc_inos " << p->second->info.prealloc_inos
	     << " used_ions " << p->second->info.used_inos
	     << dendl;
}


// ----------------
// LOAD


object_t SessionMap::get_object_name()
{
  char s[30];
  snprintf(s, sizeof(s), "mds%d_sessionmap", int(mds->whoami));
  return object_t(s);
}

class C_IO_SM_Load : public SessionMapIOContext {
public:
  const bool first;  //< Am I the initial (header) load?
  int header_r;  //< Return value from OMAP header read
  int values_r;  //< Return value from OMAP value read
  bufferlist header_bl;
  std::map<std::string, bufferlist> session_vals;

  C_IO_SM_Load(SessionMap *cm, const bool f)
    : SessionMapIOContext(cm), first(f), header_r(0), values_r(0) {}

  void finish(int r) {
    sessionmap->_load_finish(r, header_r, values_r, first, header_bl, session_vals);
  }
};


/**
 * Decode OMAP header.  Call this once when loading.
 */
void SessionMapStore::decode_header(
      bufferlist &header_bl)
{
  bufferlist::iterator q = header_bl.begin();
  DECODE_START(1, q)
  ::decode(version, q);
  DECODE_FINISH(q);
}

void SessionMapStore::encode_header(
    bufferlist *header_bl)
{
  ENCODE_START(1, 1, *header_bl);
  ::encode(version, *header_bl);
  ENCODE_FINISH(*header_bl);
}

/**
 * Decode and insert some serialized OMAP values.  Call this
 * repeatedly to insert batched loads.
 */
void SessionMapStore::decode_values(std::map<std::string, bufferlist> &session_vals)
{
  for (std::map<std::string, bufferlist>::iterator i = session_vals.begin();
       i != session_vals.end(); ++i) {

    entity_inst_t inst;

    bool parsed = inst.name.parse(i->first);
    if (!parsed) {
      derr << "Corrupt entity name '" << i->first << "' in sessionmap" << dendl;
      throw buffer::malformed_input("Corrupt entity name in sessionmap");
    }

    Session *s = get_or_add_session(inst);
    if (s->is_closed())
      s->set_state(Session::STATE_OPEN);
    bufferlist::iterator q = i->second.begin();
    s->decode(q);
  }
}

/**
 * An OMAP read finished.
 */
void SessionMap::_load_finish(
    int operation_r,
    int header_r,
    int values_r,
    bool first,
    bufferlist &header_bl,
    std::map<std::string, bufferlist> &session_vals)
{
  if (operation_r < 0) {
    derr << "_load_finish got " << cpp_strerror(operation_r) << dendl;
    assert(0 == "failed to load sessionmap");
  }

  // Decode header
  if (first) {
    if (header_r != 0) {
      derr << __func__ << ": header error: " << cpp_strerror(header_r) << dendl;
      assert(0 == "error reading header!");
    }

    if(header_bl.length() == 0) {
      dout(4) << __func__ << ": header missing, loading legacy..." << dendl;
      load_legacy();
      return;
    }

    decode_header(header_bl);
    dout(10) << __func__ << " loaded version " << version << dendl;
  }

  if (values_r != 0) {
    derr << __func__ << ": error reading values: "
      << cpp_strerror(values_r) << dendl;
    assert(0 == "error reading values");
  }

  // Decode session_vals
  decode_values(session_vals);

  if (session_vals.size() == g_conf->mds_sessionmap_keys_per_op) {
    // Issue another read if we're not at the end of the omap
    const std::string last_key = session_vals.rbegin()->first;
    dout(10) << __func__ << ": continue omap load from '"
             << last_key << "'" << dendl;
    object_t oid = get_object_name();
    object_locator_t oloc(mds->mdsmap->get_metadata_pool());
    C_IO_SM_Load *c = new C_IO_SM_Load(this, false);
    ObjectOperation op;
    op.omap_get_vals(last_key, "", g_conf->mds_sessionmap_keys_per_op,
        &c->session_vals, &c->values_r);
    mds->objecter->read(oid, oloc, op, CEPH_NOSNAP, NULL, 0,
        new C_OnFinisher(c, &mds->finisher));
  } else {
    // I/O is complete.  Update `by_state`
    dout(10) << __func__ << ": omap load complete" << dendl;
    for (ceph::unordered_map<entity_name_t, Session*>::iterator i = session_map.begin();
         i != session_map.end(); ++i) {
      Session *s = i->second;
      if (by_state.count(s->get_state()) == 0)
        by_state[s->get_state()] = new xlist<Session*>;
      by_state[s->get_state()]->push_back(&s->item_session_list);
    }

    // Population is complete.  Trigger load waiters.
    dout(10) << __func__ << ": v " << version 
	   << ", " << session_map.size() << " sessions" << dendl;
    projected = committing = committed = version;
    dump();
    finish_contexts(g_ceph_context, waiting_for_load);
  }
}

/**
 * Populate session state from OMAP records in this
 * rank's sessionmap object.
 */
void SessionMap::load(MDSInternalContextBase *onload)
{
  dout(10) << "load" << dendl;

  if (onload)
    waiting_for_load.push_back(onload);
  
  C_IO_SM_Load *c = new C_IO_SM_Load(this, true);
  object_t oid = get_object_name();
  object_locator_t oloc(mds->mdsmap->get_metadata_pool());

  ObjectOperation op;
  op.omap_get_header(&c->header_bl, &c->header_r);
  op.omap_get_vals("", "", g_conf->mds_sessionmap_keys_per_op,
      &c->session_vals, &c->values_r);

  mds->objecter->read(oid, oloc, op, CEPH_NOSNAP, NULL, 0, new C_OnFinisher(c, &mds->finisher));
}

class C_IO_SM_LoadLegacy : public SessionMapIOContext {
public:
  bufferlist bl;
  C_IO_SM_LoadLegacy(SessionMap *cm) : SessionMapIOContext(cm) {}
  void finish(int r) {
    sessionmap->_load_legacy_finish(r, bl);
  }
};


/**
 * Load legacy (object data blob) SessionMap format, assuming
 * that waiting_for_load has already been populated with
 * the relevant completion.  This is the fallback if we do not
 * find an OMAP header when attempting to load normally.
 */
void SessionMap::load_legacy()
{
  dout(10) << __func__ << dendl;

  C_IO_SM_LoadLegacy *c = new C_IO_SM_LoadLegacy(this);
  object_t oid = get_object_name();
  object_locator_t oloc(mds->mdsmap->get_metadata_pool());

  mds->objecter->read_full(oid, oloc, CEPH_NOSNAP, &c->bl, 0,
			   new C_OnFinisher(c, &mds->finisher));
}

void SessionMap::_load_legacy_finish(int r, bufferlist &bl)
{ 
  bufferlist::iterator blp = bl.begin();
  if (r < 0) {
    derr << "_load_finish got " << cpp_strerror(r) << dendl;
    assert(0 == "failed to load sessionmap");
  }
  dump();
  decode_legacy(blp);  // note: this sets last_cap_renew = now()
  dout(10) << "_load_finish v " << version 
	   << ", " << session_map.size() << " sessions, "
	   << bl.length() << " bytes"
	   << dendl;
  projected = committing = committed = version;
  dump();

  // Mark all sessions dirty, so that on next save() we will write
  // a complete OMAP version of the data loaded from the legacy format
  for (ceph::unordered_map<entity_name_t, Session*>::iterator i = session_map.begin();
       i != session_map.end(); ++i) {
    // Don't use mark_dirty because on this occasion we want to ignore the
    // keys_per_op limit and do one big write (upgrade must be atomic)
    dirty_sessions.insert(i->first);
  }
  loaded_legacy = true;

  finish_contexts(g_ceph_context, waiting_for_load);
}


// ----------------
// SAVE

class C_IO_SM_Save : public SessionMapIOContext {
  version_t version;
public:
  C_IO_SM_Save(SessionMap *cm, version_t v) : SessionMapIOContext(cm), version(v) {}
  void finish(int r) {
    assert(r == 0);
    sessionmap->_save_finish(version);
  }
};

void SessionMap::save(MDSInternalContextBase *onsave, version_t needv)
{
  dout(10) << __func__ << ": needv " << needv << ", v " << version << dendl;
 
  if (needv && committing >= needv) {
    assert(committing > committed);
    commit_waiters[committing].push_back(onsave);
    return;
  }

  commit_waiters[version].push_back(onsave);

  committing = version;
  SnapContext snapc;
  object_t oid = get_object_name();
  object_locator_t oloc(mds->mdsmap->get_metadata_pool());

  ObjectOperation op;

  /* Compose OSD OMAP transaction for full write */
  bufferlist header_bl;
  encode_header(&header_bl);
  op.omap_set_header(header_bl);

  /* If we loaded a legacy sessionmap, then erase the old data.  If
   * an old-versioned MDS tries to read it, it'll fail out safely
   * with an end_of_buffer exception */
  if (loaded_legacy) {
    dout(4) << __func__ << " erasing legacy sessionmap" << dendl;
    op.truncate(0);
    loaded_legacy = false;  // only need to truncate once.
  }

  dout(20) << " updating keys:" << dendl;
  map<string, bufferlist> to_set;
  for(std::set<entity_name_t>::const_iterator i = dirty_sessions.begin();
      i != dirty_sessions.end(); ++i) {
    const entity_name_t name = *i;
    const Session *session = session_map[name];

    if (session->is_open() ||
	session->is_closing() ||
	session->is_stale() ||
	session->is_killing()) {
      dout(20) << "  " << name << dendl;
      // Serialize K
      std::ostringstream k;
      k << name;

      // Serialize V
      bufferlist bl;
      session->info.encode(bl);

      // Add to RADOS op
      to_set[k.str()] = bl;
    } else {
      dout(20) << "  " << name << " (ignoring)" << dendl;
    }
  }
  if (!to_set.empty()) {
    op.omap_set(to_set);
  }

  dout(20) << " removing keys:" << dendl;
  set<string> to_remove;
  for(std::set<entity_name_t>::const_iterator i = null_sessions.begin();
      i != null_sessions.end(); ++i) {
    dout(20) << "  " << *i << dendl;
    std::ostringstream k;
    k << *i;
    to_remove.insert(k.str());
  }
  if (!to_remove.empty()) {
    op.omap_rm_keys(to_remove);
  }

  dirty_sessions.clear();
  null_sessions.clear();

  mds->objecter->mutate(oid, oloc, op, snapc, ceph_clock_now(g_ceph_context),
      0, NULL, new C_OnFinisher(new C_IO_SM_Save(this, version), &mds->finisher));
}

void SessionMap::_save_finish(version_t v)
{
  dout(10) << "_save_finish v" << v << dendl;
  committed = v;

  finish_contexts(g_ceph_context, commit_waiters[v]);
  commit_waiters.erase(v);
}


/**
 * Deserialize sessions, and update by_state index
 */
void SessionMap::decode_legacy(bufferlist::iterator &p)
{
  // Populate `sessions`
  SessionMapStore::decode_legacy(p);

  // Update `by_state`
  for (ceph::unordered_map<entity_name_t, Session*>::iterator i = session_map.begin();
       i != session_map.end(); ++i) {
    Session *s = i->second;
    if (by_state.count(s->get_state()) == 0)
      by_state[s->get_state()] = new xlist<Session*>;
    by_state[s->get_state()]->push_back(&s->item_session_list);
  }
}

uint64_t SessionMap::set_state(Session *session, int s) {
  if (session->state != s) {
    session->set_state(s);
    if (by_state.count(s) == 0)
      by_state[s] = new xlist<Session*>;
    by_state[s]->push_back(&session->item_session_list);
  }
  return session->get_state_seq();
}

void SessionMapStore::decode_legacy(bufferlist::iterator& p)
{
  utime_t now = ceph_clock_now(g_ceph_context);
  uint64_t pre;
  ::decode(pre, p);
  if (pre == (uint64_t)-1) {
    DECODE_START_LEGACY_COMPAT_LEN(3, 3, 3, p);
    assert(struct_v >= 2);
    
    ::decode(version, p);
    
    while (!p.end()) {
      entity_inst_t inst;
      ::decode(inst.name, p);
      Session *s = get_or_add_session(inst);
      if (s->is_closed())
        s->set_state(Session::STATE_OPEN);
      s->decode(p);
    }

    DECODE_FINISH(p);
  } else {
    // --- old format ----
    version = pre;

    // this is a meaningless upper bound.  can be ignored.
    __u32 n;
    ::decode(n, p);
    
    while (n-- && !p.end()) {
      bufferlist::iterator p2 = p;
      Session *s = new Session;
      s->info.decode(p);
      if (session_map.count(s->info.inst.name)) {
	// eager client connected too fast!  aie.
	dout(10) << " already had session for " << s->info.inst.name << ", recovering" << dendl;
	entity_name_t n = s->info.inst.name;
	delete s;
	s = session_map[n];
	p = p2;
	s->info.decode(p);
      } else {
	session_map[s->info.inst.name] = s;
      }
      s->set_state(Session::STATE_OPEN);
      s->last_cap_renew = now;
    }
  }
}

void SessionMapStore::dump(Formatter *f) const
{
  f->open_array_section("Sessions");
  for (ceph::unordered_map<entity_name_t,Session*>::const_iterator p = session_map.begin();
       p != session_map.end();
       ++p)  {
    f->open_object_section("Session");
    f->open_object_section("entity name");
    p->first.dump(f);
    f->close_section(); // entity name
    f->dump_string("state", p->second->get_state_name());
    f->open_object_section("Session info");
    p->second->info.dump(f);
    f->close_section(); // Session info
    f->close_section(); // Session
  }
  f->close_section(); // Sessions
}

void SessionMapStore::generate_test_instances(list<SessionMapStore*>& ls)
{
  // pretty boring for now
  ls.push_back(new SessionMapStore());
}

void SessionMap::wipe()
{
  dout(1) << "wipe start" << dendl;
  dump();
  while (!session_map.empty()) {
    Session *s = session_map.begin()->second;
    remove_session(s);
  }
  version = ++projected;
  dout(1) << "wipe result" << dendl;
  dump();
  dout(1) << "wipe done" << dendl;
}

void SessionMap::wipe_ino_prealloc()
{
  for (ceph::unordered_map<entity_name_t,Session*>::iterator p = session_map.begin(); 
       p != session_map.end(); 
       ++p) {
    p->second->pending_prealloc_inos.clear();
    p->second->info.prealloc_inos.clear();
    p->second->info.used_inos.clear();
  }
  projected = ++version;
}

/**
 * Calculate the length of the `requests` member list,
 * because elist does not have a size() method.
 *
 * O(N) runtime.  This would be const, but elist doesn't
 * have const iterators.
 */
size_t Session::get_request_count()
{
  size_t result = 0;

  elist<MDRequestImpl*>::iterator p = requests.begin(
      member_offset(MDRequestImpl, item_session_request));
  while (!p.end()) {
    ++result;
    ++p;
  }

  return result;
}

void SessionMap::add_session(Session *s)
{
  dout(10) << __func__ << " s=" << s << " name=" << s->info.inst.name << dendl;

  assert(session_map.count(s->info.inst.name) == 0);
  session_map[s->info.inst.name] = s;
  if (by_state.count(s->state) == 0)
    by_state[s->state] = new xlist<Session*>;
  by_state[s->state]->push_back(&s->item_session_list);
  s->get();
}

void SessionMap::remove_session(Session *s)
{
  dout(10) << __func__ << " s=" << s << " name=" << s->info.inst.name << dendl;

  s->trim_completed_requests(0);
  s->item_session_list.remove_myself();
  session_map.erase(s->info.inst.name);
  s->put();
  if (dirty_sessions.count(s->info.inst.name)) {
    dirty_sessions.erase(s->info.inst.name);
  }
  null_sessions.insert(s->info.inst.name);
}

void SessionMap::touch_session(Session *session)
{
  dout(10) << __func__ << " s=" << session << " name=" << session->info.inst.name << dendl;

  // Move to the back of the session list for this state (should
  // already be on a list courtesy of add_session and set_state)
  assert(session->item_session_list.is_on_list());
  if (by_state.count(session->state) == 0)
    by_state[session->state] = new xlist<Session*>;
  by_state[session->state]->push_back(&session->item_session_list);

  session->last_cap_renew = ceph_clock_now(g_ceph_context);
}

/**
 * Capped in response to a CEPH_MSG_CLIENT_CAPRELEASE message,
 * with n_caps equal to the number of caps that were released
 * in the message.  Used to update state about how many caps a
 * client has released since it was last instructed to RECALL_STATE.
 */
void Session::notify_cap_release(size_t n_caps)
{
  if (!recalled_at.is_zero()) {
    recall_release_count += n_caps;
    if (recall_release_count >= recall_count) {
      recalled_at = utime_t();
      recall_count = 0;
      recall_release_count = 0;
    }
  }
}

/**
 * Called when a CEPH_MSG_CLIENT_SESSION->CEPH_SESSION_RECALL_STATE
 * message is sent to the client.  Update our recall-related state
 * in order to generate health metrics if the session doesn't see
 * a commensurate number of calls to ::notify_cap_release
 */
void Session::notify_recall_sent(int const new_limit)
{
  if (recalled_at.is_zero()) {
    // Entering recall phase, set up counters so we can later
    // judge whether the client has respected the recall request
    recalled_at = ceph_clock_now(g_ceph_context);
    assert (new_limit < caps.size());  // Behaviour of Server::recall_client_state
    recall_count = caps.size() - new_limit;
    recall_release_count = 0;
  }
}

void Session::set_client_metadata(map<string, string> const &meta)
{
  info.client_metadata = meta;

  _update_human_name();
}

/**
 * Use client metadata to generate a somewhat-friendlier
 * name for the client than its session ID.
 *
 * This is *not* guaranteed to be unique, and any machine
 * consumers of session-related output should always use
 * the session ID as a primary capacity and use this only
 * as a presentation hint.
 */
void Session::_update_human_name()
{
  if (info.client_metadata.count("hostname")) {
    // Happy path, refer to clients by hostname
    human_name = info.client_metadata["hostname"];
    if (info.client_metadata.count("entity_id")) {
      EntityName entity;
      entity.set_id(info.client_metadata["entity_id"]);
      if (!entity.has_default_id()) {
        // When a non-default entity ID is set by the user, assume they
        // would like to see it in references to the client
        human_name += std::string(":") + entity.get_id();
      }
    }
  } else {
    // Fallback, refer to clients by ID e.g. client.4567
    human_name = stringify(info.inst.name.num());
  }
}

void Session::decode(bufferlist::iterator &p)
{
  info.decode(p);

  _update_human_name();
}

void SessionMap::_mark_dirty(Session *s)
{
  if (dirty_sessions.size() >= g_conf->mds_sessionmap_keys_per_op) {
    // Pre-empt the usual save() call from journal segment trim, in
    // order to avoid building up an oversized OMAP update operation
    // from too many sessions modified at once
    save(new C_MDSInternalNoop, version);
  }

  dirty_sessions.insert(s->info.inst.name);
}

void SessionMap::mark_dirty(Session *s)
{
  dout(20) << __func__ << " s=" << s << " name=" << s->info.inst.name
    << " v=" << version << dendl;

  _mark_dirty(s);
  version++;
  s->pop_pv(version);
}

void SessionMap::replay_dirty_session(Session *s)
{
  dout(20) << __func__ << " s=" << s << " name=" << s->info.inst.name
    << " v=" << version << dendl;

  _mark_dirty(s);

  replay_advance_version();
}

void SessionMap::replay_advance_version()
{
  version++;
  projected = version;
}

version_t SessionMap::mark_projected(Session *s)
{
  dout(20) << __func__ << " s=" << s << " name=" << s->info.inst.name
    << " pv=" << projected << " -> " << projected + 1 << dendl;
  ++projected;
  s->push_pv(projected);
  return projected;
}

