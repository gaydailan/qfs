/*!
 * $Id$
 *
 * \file Replay.cc
 * \brief transaction log replay
 * \author Blake Lewis (Kosmix Corp.)
 *         Mike Ovsiannikov
 *
 * Copyright 2008-2012 Quantcast Corp.
 * Copyright 2006-2008 Kosmix Corp.
 *
 * This file is part of Kosmos File System (KFS).
 *
 * Licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "Replay.h"
#include "LogWriter.h"
#include "Restorer.h"
#include "util.h"
#include "DiskEntry.h"
#include "kfstree.h"
#include "LayoutManager.h"
#include "MetaVrSM.h"
#include "MetaVrOps.h"
#include "MetaDataStore.h"

#include "common/MdStream.h"
#include "common/MsgLogger.h"
#include "common/StdAllocator.h"
#include "common/kfserrno.h"
#include "common/RequestParser.h"
#include "common/juliantime.h"
#include "common/StBuffer.h"

#include "kfsio/checksum.h"

#include "qcdio/QCUtils.h"

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include <cassert>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <deque>
#include <set>

namespace KFS
{
using std::ostringstream;
using std::deque;
using std::set;
using std::less;
using std::hex;
using std::dec;

inline void
Replay::setRollSeeds(int64_t roll)
{
    rollSeeds = roll;
}

class ReplayState
{
public:
    class CommitQueueEntry
    {
    public:
        CommitQueueEntry(
            const MetaVrLogSeq& ls = MetaVrLogSeq(),
            int                 s  = 0,
            fid_t               fs = -1,
            int64_t             ek = 0,
            MetaRequest*        o  = 0)
            : logSeq(ls),
              seed(fs),
              errChecksum(ek),
              status(s),
              op(o)
            {}
        MetaVrLogSeq logSeq;
        fid_t        seed;
        int64_t      errChecksum;
        int          status;
        MetaRequest* op;
    };

    typedef deque<
        CommitQueueEntry
    > CommitQueue;

    ReplayState(
        Replay* replay)
        : mCommitQueue(),
          mCheckpointCommitted(),
          mCheckpointErrChksum(-1),
          mLastCommitted(),
          mBlockStartLogSeq(),
          mLastBlockSeq(-1),
          mLastLogAheadSeq(),
          mLogAheadErrChksum(0),
          mSubEntryCount(0),
          mLogSegmentTimeUsec(0),
          mLastCommittedStatus(0),
          mRestoreTimeCount(0),
          mReplayer(replay),
          mCurOp(0)
        {}
    ~ReplayState()
    {
        curOpDone();
    }
    bool runCommitQueue(
        const MetaVrLogSeq& logSeq,
        seq_t               seed,
        int64_t             status,
        int64_t             errChecksum);
    bool incSeq()
    {
        if (0 != mSubEntryCount) {
            return false;
        }
        curOpDone();
        mLastLogAheadSeq.mLogSeq++;
        return true;
    }
    bool subEntry()
    {
        if (--mSubEntryCount <= 0) {
            return incSeq();
        }
        return true;
    }
    static ReplayState& get(const DETokenizer& c)
        { return *reinterpret_cast<ReplayState*>(c.getUserData()); }
    void handle()
    {
        if (! mCurOp) {
            panic("replay: no current op");
            return;
        }
        if (mReplayer) {
            mCommitQueue.push_back(
                ReplayState::CommitQueueEntry(mCurOp->logseq, 0, 0, 0, mCurOp));
            mCurOp = 0;
        } else {
            handleOp(*mCurOp);
        }
    }
    bool IsCurOpLogSeqValid() const
    {
        if (! mReplayer || ! mCurOp) {
            return true;
        }
        MetaVrLogSeq next = mLastLogAheadSeq;
        next.mLogSeq++;
        if (next == mCurOp->logseq || META_VR_LOG_START_VIEW == mCurOp->op) {
            return true;
        }
        KFS_LOG_STREAM_ERROR <<
            "replay logseq mismatch:"
            " expected: "  << next <<
            " actual: "    << mCurOp->logseq <<
            " "            << mCurOp->Show() <<
        KFS_LOG_EOM;
        return false;
    }
    void replayCurOp()
    {
        if (! mCurOp || 1 != mSubEntryCount) {
            panic("invalid replay current op invocation");
            return;
        }
        if (! IsCurOpLogSeqValid()) {
            panic("invalid current op log sequence");
            return;
        }
        handle();
    }
    void commit(CommitQueueEntry& entry)
    {
        if (! entry.op) {
            return;
        }
        MetaRequest& op = *entry.op;
        handleOp(op);
        const int status = op.status < 0 ? SysToKfsErrno(-op.status) : 0;
        mLastCommittedStatus = status;
        mLogAheadErrChksum  += status;
        entry.logSeq      = op.logseq;
        entry.seed        = fileID.getseed();
        entry.errChecksum = mLogAheadErrChksum;
        entry.status      = status;
        entry.op          = 0;
        if (&op != mCurOp && ! op.suspended) {
            opDone(op);
        }
    }
    void handleOp(MetaRequest& op)
    {
        if (op.replayFlag) {
            op.seqno = MetaRequest::GetLogWriter().GetNextSeq();
        }
        op.handle();
        KFS_LOG_STREAM_DEBUG <<
            (mReplayer ? (op.replayFlag ? "replay:" : "commit") : "handle:") <<
            " logseq: " << op.logseq <<
            " "         << hex << op.logseq << dec <<
            " status: " << op.status <<
            " "         << op.statusMsg <<
            " "         << op.Show() <<
        KFS_LOG_EOM;
        if (op.suspended && &op == mCurOp) {
            mCurOp = 0;
        }
    }
    bool commmitAll()
    {
        if (0 != mSubEntryCount) {
            return false;
        }
        if (mCommitQueue.empty()) {
            return true;;
        }
        for (CommitQueue::iterator it = mCommitQueue.begin();
                mCommitQueue.end() != it;
                ++it) {
            commit(*it);
        }
        mBlockStartLogSeq = mCommitQueue.back().logSeq;
        mLastCommitted    = mBlockStartLogSeq;
        mCommitQueue.clear();
        return true;
    }
    void curOpDone()
    {
        if (! mCurOp) {
            return;
        }
        MetaRequest& op = *mCurOp;
        mCurOp = 0;
        opDone(op);
    }
    static void opDone(MetaRequest& op)
    {
        if (op.replayFlag) {
            op.replayFlag = false;
            MetaRequest::Release(&op);
        } else {
            submit_request(&op);
        }
    }
    void handleStartView(MetaVrLogStartView& op)
    {
    }
    bool setReplayState(
        const MetaVrLogSeq& committed,
        int64_t             errChecksum,
        int                 lastCommittedStatus,
        MetaRequest*        commitQueue)
    {
        if (mCurOp || ! mReplayer) {
            return false;
        }
        if (! mCommitQueue.empty() && ! runCommitQueue(
                committed,
                fileID.getseed(),
                lastCommittedStatus,
                errChecksum)) {
            return false;
        }
        mLastCommitted       = committed;
        mBlockStartLogSeq    = committed;
        mLastLogAheadSeq     = committed;
        mLogAheadErrChksum   = errChecksum;
        mLastCommittedStatus = lastCommittedStatus;
        mLogAheadErrChksum   = errChecksum;
        mSubEntryCount       = 0;
        MetaRequest* next = commitQueue;
        while (next) {
            MetaRequest& op = *next;
            next = op.next;
            op.next = 0;
            mLastLogAheadSeq = op.logseq;
            mCommitQueue.push_back(
                ReplayState::CommitQueueEntry(op.logseq, 0, 0, 0, &op));
        }
        return true;
    }

    CommitQueue   mCommitQueue;
    MetaVrLogSeq  mCheckpointCommitted;
    seq_t         mCheckpointErrChksum;
    MetaVrLogSeq  mLastCommitted;
    MetaVrLogSeq  mBlockStartLogSeq;
    seq_t         mLastBlockSeq;
    MetaVrLogSeq  mLastLogAheadSeq;
    int64_t       mLogAheadErrChksum;
    int64_t       mSubEntryCount;
    int64_t       mLogSegmentTimeUsec;
    int           mLastCommittedStatus;
    int           mRestoreTimeCount;
    Replay* const mReplayer;
    MetaRequest*  mCurOp;
private:
    ReplayState(const ReplayState&);
    ReplayState& operator=(const ReplayState&);
};

class Replay::ReplayState : public KFS::ReplayState
{
public:
    ReplayState(
        Replay* replay)
        : KFS::ReplayState(replay)
        {}
};

static bool
parse_vr_log_seq(DETokenizer& c, MetaVrLogSeq& outSeq)
{
    if (c.empty()) {
        return false;
    }
    const char* cur = c.front().ptr;
    return (
        16 == c.getIntBase() ?
        outSeq.Parse<HexIntParser>(cur, c.front().len) :
        outSeq.Parse<DecIntParser>(cur, c.front().len)
    );
}

/*!
 * \brief open saved log file for replay
 * \param[in] p a path in the form "<logdir>/log.<number>"
 */
int
Replay::openlog(const string& name)
{
    if (file.is_open()) {
        file.close();
    }
    string::size_type pos = name.rfind('/');
    if (string::npos == pos) {
        pos = 0;
    } else {
        pos++;
    }
    if (logdir.empty()) {
        tmplogname.assign(name.data(), name.size());
    } else {
        tmplogname.assign(logdir.data(), logdir.size());
        tmplogname += name.data() + pos;
        pos = logdir.size();
    }
    KFS_LOG_STREAM_INFO <<
        "open log file: " << name << " => " << tmplogname <<
    KFS_LOG_EOM;
    int64_t                 num = -1;
    const string::size_type dot = tmplogname.rfind('.');
    if (string::npos == dot || dot < pos ||
            (num = toNumber(tmplogname.c_str() + dot + 1)) < 0) {
        KFS_LOG_STREAM_FATAL <<
            tmplogname << ": invalid log file name" <<
        KFS_LOG_EOM;
        tmplogname.clear();
        return -EINVAL;
    }
    file.open(tmplogname.c_str(), ifstream::in | ifstream::binary);
    if (file.fail()) {
        const int err = errno;
        KFS_LOG_STREAM_FATAL <<
            tmplogname << ": " << QCUtils::SysError(err) <<
        KFS_LOG_EOM;
        tmplogname.clear();
        return (err > 0 ? -err : (err == 0 ? -1 : err));
    }
    number = num;
    path.assign(tmplogname.data(), tmplogname.size());
    tmplogname.clear();
    return 0;
}

void
Replay::setLogDir(const char* dir)
{
    if (dir && *dir) {
        logdir = dir;
        if ('/' != *logdir.rbegin()) {
            logdir += '/';
        }
    } else {
        logdir.clear();
    }
}

const string&
Replay::logfile(seq_t num)
{
    if (tmplogname.empty()) {
        string::size_type name = path.rfind('/');
        if (string::npos == name) {
            name = 0;
        } else {
            name++;
        }
        const string::size_type dot = path.find('.', name);
        if (dot == string::npos) {
            tmplogname = path + ".";
        } else {
            tmplogname = path.substr(0, dot + 1);
        }
        tmplogprefixlen = tmplogname.length();
    }
    tmplogname.erase(tmplogprefixlen);
    if (logSegmentHasLogSeq(num)) {
        AppendDecIntToString(tmplogname, committed.mEpochSeq);
        tmplogname += '.';
        AppendDecIntToString(tmplogname, committed.mViewSeq);
        tmplogname += '.';
        AppendDecIntToString(tmplogname, committed.mLogSeq);
        tmplogname += '.';
    }
    AppendDecIntToString(tmplogname, num);
    return tmplogname;
}

string
Replay::getLastLog()
{
    const char* kLast = MetaDataStore::GetLogSegmentLastFileNamePtr();
    const string::size_type pos = path.rfind('/');
    if (string::npos != pos) {
        return path.substr(0, pos + 1) + kLast;
    }
    return kLast;
}

/*!
 * \brief check log version
 * format: version/<number>
 */
static bool
replay_version(DETokenizer& c)
{
    fid_t vers;
    bool ok = pop_fid(vers, "version", c, true);
    return (ok && vers == LogWriter::VERSION);
}

/*!
 * \brief handle common prefix for all log records
 */
static bool
pop_parent(fid_t &id, DETokenizer& c)
{
    c.pop_front();      // get rid of record type
    return pop_fid(id, "dir", c, true);
}

/*!
 * \brief update the seed of a UniqueID with what is passed in.
 * Since this function is called in the context of log replay, it
 * better be the case that the seed passed in is higher than
 * the id's seed (which was set from a checkpoint file).
*/
static void
updateSeed(UniqueID &id, seqid_t seed)
{
    if (seed < id.getseed()) {
        ostringstream os;
        os << "seed from log: " << seed <<
            " < id's seed: " << id.getseed();
        panic(os.str(), false);
    }
    id.setseed(seed);
}

/*!
 * \brief replay a file create
 * format: create/dir/<parentID>/name/<name>/id/<myID>{/ctime/<time>}
 */
static bool
replay_create(DETokenizer& c)
{
    fid_t parent, me;
    string myname;
    int status = 0;
    int16_t numReplicas;
    int64_t ctime;

    bool ok = pop_parent(parent, c);
    ok = pop_name(myname, "name", c, ok);
    ok = pop_fid(me, "id", c, ok);
    ok = pop_short(numReplicas, "numReplicas", c, ok);
    // if the log has the ctime, pass it thru
    const bool gottime = pop_time(ctime, "ctime", c, ok);
    chunkOff_t t = KFS_STRIPED_FILE_TYPE_NONE, n = 0, nr = 0, ss = 0;
    if (ok && gottime && pop_offset(t, "striperType", c, true)) {
        ok =    pop_offset(n,  "numStripes",         c, true) &&
            pop_offset(nr, "numRecoveryStripes", c, true) &&
            pop_offset(ss, "stripeSize",         c, true);
    }
    if (! ok) {
        return false;
    }
    fid_t todumpster = -1;
    if (! pop_fid(todumpster, "todumpster", c, ok)) {
        todumpster = -1;
    }
    kfsUid_t   user     = kKfsUserNone;
    kfsUid_t   group    = kKfsGroupNone;
    kfsMode_t  mode     = 0;
    int64_t    k        = user;
    kfsSTier_t minSTier = kKfsSTierMax;
    kfsSTier_t maxSTier = kKfsSTierMax;
    if (! c.empty()) {
        if (! pop_num(k, "user", c, ok)) {
            return false;
        }
        user = (kfsUid_t)k;
        if (user == kKfsUserNone) {
            return false;
        }
        k = group;
        if (! pop_num(k, "group", c, ok)) {
            return false;
        }
        group = (kfsGid_t)k;
        if (group == kKfsGroupNone) {
            return false;
        }
        k = mode;
        if (! pop_num(k, "mode", c, ok)) {
            return false;
        }
        mode = (kfsMode_t)k;
        if (! c.empty()) {
            if (! pop_num(k, "minTier", c, ok)) {
                return false;
            }
            minSTier = (kfsSTier_t)k;
            if (! pop_num(k, "maxTier", c, ok)) {
                return false;
            }
            maxSTier = (kfsSTier_t)k;
        }
    } else {
        user  = gLayoutManager.GetDefaultLoadUser();
        group = gLayoutManager.GetDefaultLoadGroup();
        mode  = gLayoutManager.GetDefaultLoadFileMode();
    }
    if (user == kKfsUserNone || group == kKfsGroupNone ||
            mode == kKfsModeUndef) {
        return false;
    }
    if (maxSTier < minSTier ||
            ! IsValidSTier(minSTier) ||
            ! IsValidSTier(maxSTier)) {
        return false;
    }
    // for all creates that were successful during normal operation,
    // when we replay it should work; so, exclusive = false
    MetaFattr* fa = 0;
    status = metatree.create(parent, myname, &me, numReplicas, false,
        t, n, nr, ss, todumpster, user, group, mode,
        kKfsUserRoot, kKfsGroupRoot, &fa,
        gottime ? ctime : ReplayState::get(c).mLogSegmentTimeUsec);
    if (status == 0) {
        assert(fa);
        updateSeed(fileID, me);
        if (gottime) {
            fa->mtime = fa->ctime = fa->crtime = ctime;
            if (fa->IsStriped()) {
                fa->filesize = 0;
            }
        }
        if (minSTier < kKfsSTierMax) {
            fa->minSTier = minSTier;
            fa->maxSTier = maxSTier;
        }
    }
    KFS_LOG_STREAM_DEBUG << "replay create:"
        " name: " << myname <<
        " id: "   << me <<
    KFS_LOG_EOM;
    return (status == 0);
}

/*!
 * \brief replay mkdir
 * format: mkdir/dir/<parentID>/name/<name>/id/<myID>{/ctime/<time>}
 */
static bool
replay_mkdir(DETokenizer& c)
{
    fid_t parent, me;
    string myname;
    int status = 0;
    int64_t ctime;

    bool ok = pop_parent(parent, c);
    ok = pop_name(myname, "name", c, ok);
    ok = pop_fid(me, "id", c, ok);
    if (! ok) {
        return false;
    }
    // if the log has the ctime, pass it thru
    const bool gottime = pop_time(ctime, "ctime", c, ok);
    kfsUid_t  user  = kKfsUserNone;
    kfsUid_t  group = kKfsGroupNone;
    kfsMode_t mode  = 0;
    int64_t   k     = user;
    if (pop_num(k, "user", c, ok)) {
        user = (kfsUid_t)k;
        if (user == kKfsUserNone) {
            return false;
        }
        k = group;
        if (! pop_num(k, "group", c, ok)) {
            return false;
        }
        group = (kfsGid_t)k;
        if (group == kKfsGroupNone) {
            return false;
        }
        k = mode;
        if (! pop_num(k, "mode", c, ok)) {
            return false;
        }
        mode = (kfsMode_t)k;
    } else {
        user  = gLayoutManager.GetDefaultLoadUser();
        group = gLayoutManager.GetDefaultLoadGroup();
        mode  = gLayoutManager.GetDefaultLoadFileMode();
    }
    if (user == kKfsUserNone || group == kKfsGroupNone ||
            mode == kKfsModeUndef) {
        return false;
    }
    int64_t mtime;
    if (! pop_time(mtime, "mtime", c, ok)) {
        mtime = ReplayState::get(c).mLogSegmentTimeUsec;
    }
    MetaFattr* fa = 0;
    status = metatree.mkdir(parent, myname, user, group, mode,
        kKfsUserRoot, kKfsGroupRoot, &me, &fa, mtime);
    if (status == 0) {
        assert(fa);
        updateSeed(fileID, me);
        if (gottime) {
            fa->mtime = fa->ctime = fa->crtime = ctime;
        }
    }
    KFS_LOG_STREAM_DEBUG << "replay mkdir: "
        " name: " << myname <<
        " id: "   << me <<
    KFS_LOG_EOM;
    return (ok && status == 0);
}

/*!
 * \brief replay remove
 * format: remove/dir/<parentID>/name/<name>
 */
static bool
replay_remove(DETokenizer& c)
{
    fid_t parent;
    string myname;
    int status = 0;
    bool ok = pop_parent(parent, c);
    ok = pop_name(myname, "name", c, ok);
    fid_t todumpster = -1;
    if (! pop_fid(todumpster, "todumpster", c, ok)) {
        todumpster = -1;
    }
    if (ok) {
        int64_t mtime;
        if (! pop_time(mtime, "mtime", c, ok)) {
            mtime = ReplayState::get(c).mLogSegmentTimeUsec;
        }
        status = metatree.remove(parent, myname, "", todumpster,
        kKfsUserRoot, kKfsGroupRoot, mtime);
    }
    return (ok && status == 0);
}

/*!
 * \brief replay rmdir
 * format: rmdir/dir/<parentID>/name/<name>
 */
static bool
replay_rmdir(DETokenizer& c)
{
    fid_t parent;
    string myname;
    int status = 0;
    bool ok = pop_parent(parent, c);
    ok = pop_name(myname, "name", c, ok);
    if (ok) {
        int64_t mtime;
        if (! pop_time(mtime, "mtime", c, ok)) {
            mtime = ReplayState::get(c).mLogSegmentTimeUsec;
        }
        status = metatree.rmdir(parent, myname, "",
            kKfsUserRoot, kKfsGroupRoot, mtime);
    }
    return (ok && status == 0);
}

/*!
 * \brief replay rename
 * format: rename/dir/<parentID>/old/<oldname>/new/<newpath>
 * NOTE: <oldname> is the name of file/dir in parent.  This
 * will never contain any slashes.
 * <newpath> is the full path of file/dir. This may contain slashes.
 * Since it is the last component, everything after new is <newpath>.
 * So, unlike <oldname> which just requires taking one element out,
 * we need to take everything after "new" for the <newpath>.
 *
 */
static bool
replay_rename(DETokenizer& c)
{
    fid_t parent;
    string oldname, newpath;
    int status = 0;
    bool ok = pop_parent(parent, c);
    ok = pop_name(oldname, "old", c, ok);
    ok = pop_path(newpath, "new", c, ok);
    fid_t todumpster = -1;
    if (! pop_fid(todumpster, "todumpster", c, ok))
        todumpster = -1;
    if (ok) {
        int64_t mtime;
        if (! pop_time(mtime, "mtime", c, ok)) {
            mtime = ReplayState::get(c).mLogSegmentTimeUsec;
        }
        string oldpath;
        status = metatree.rename(parent, oldname, newpath, oldpath,
            true, todumpster, kKfsUserRoot, kKfsGroupRoot, mtime);
    }
    return (ok && status == 0);
}

/*!
 * \brief replay allocate
 * format: allocate/file/<fileID>/offset/<offset>/chunkId/<chunkID>/
 * chunkVersion/<chunkVersion>/{mtime/<time>}{/append/<1|0>}
 */
static bool
replay_allocate(DETokenizer& c)
{
    fid_t fid;
    chunkId_t cid, logChunkId;
    chunkOff_t offset, tmp = 0;
    seq_t chunkVersion = -1, logChunkVersion;
    int status = 0;
    int64_t mtime;

    c.pop_front();
    bool ok = pop_fid(fid, "file", c, true);
    ok = pop_fid(offset, "offset", c, ok);
    ok = pop_fid(logChunkId, "chunkId", c, ok);
    ok = pop_fid(logChunkVersion, "chunkVersion", c, ok);
    // if the log has the mtime, pass it thru
    const bool gottime = pop_time(mtime, "mtime", c, ok);
    const bool append = pop_fid(tmp, "append", c, ok) && tmp != 0;

    // during normal operation, if a file that has a valid
    // lease is removed, we move the file to the dumpster and log it.
    // a subsequent allocation on that file will succeed.
    // the remove/allocation is recorded in the logs in that order.
    // during replay, we do the remove first and then we try to
    // replay allocation; for the allocation, we won't find
    // the file attributes.  we move on...when the chunkservers
    // that has the associated chunks for the file contacts us, we won't
    // find the fid and so those chunks will get nuked as stale.
    MetaFattr* const fa = metatree.getFattr(fid);
    if (! fa) {
        return ok;
    }
    if (ok) {
        // if the log has the mtime, set it up in the FA
        if (gottime) {
            fa->mtime = max(fa->mtime, mtime);
        }
        cid = logChunkId;
        bool stripedFile = false;
        status = metatree.allocateChunkId(fid, offset, &cid,
                        &chunkVersion, NULL, &stripedFile);
        if (stripedFile && append) {
            return false; // append is not supported with striped files
        }
        const bool chunkExists = status == -EEXIST;
        if (chunkExists) {
            if (cid != logChunkId) {
                return false;
            }
            if (chunkVersion == logChunkVersion) {
                return true;
            }
            status = 0;
        }
        if (status == 0) {
            assert(cid == logChunkId);
            status = metatree.assignChunkId(fid, offset,
                            cid, logChunkVersion, 0, 0, append);
            if (status == 0) {
                fid_t cfid = 0;
                if (chunkExists &&
                        (! gLayoutManager.GetChunkFileId(
                            cid, cfid) ||
                        fid != cfid)) {
                    panic("missing chunk mapping", false);
                }
                MetaLogChunkAllocate logAlloc;
                logAlloc.replayFlag          = true;
                logAlloc.status              = 0;
                logAlloc.fid                 = fid;
                logAlloc.offset              = offset;
                logAlloc.chunkId             = logChunkId;
                logAlloc.chunkVersion        = logChunkVersion;
                logAlloc.appendChunk         = append;
                logAlloc.invalidateAllFlag   = false;
                logAlloc.objectStoreFileFlag = 0 == fa->numReplicas;
                logAlloc.initialChunkVersion = chunkVersion;
                logAlloc.mtime               = gottime ? mtime : fa->mtime;
                gLayoutManager.CommitOrRollBackChunkVersion(logAlloc);
                status = logAlloc.status;
                // assign updates the mtime; so, set it to what is in the log.
                if (0 == status && gottime) {
                    fa->mtime = mtime;
                }
                if (cid > chunkID.getseed()) {
                    // chunkID are handled by a two-stage
                    // allocation: the seed is updated in
                    // the first part of the allocation and
                    // the chunk is attached to the file
                    // after the chunkservers have ack'ed
                    // the allocation.  We can have a run
                    // where: (1) the seed is updated, (2)
                    // a checkpoint is taken, (3) allocation
                    // is done and written to log file.  If
                    // we crash, then the cid in log < seed in ckpt.
                    updateSeed(chunkID, cid);
                }
            }
        }
    }
    return (ok && status == 0);
}

/*!
 * \brief replay coalesce (do the cleanup/accounting actions)
 * format: coalesce/old/<srcFid>/new/<dstFid>/count/<# of blocks coalesced>
 */
static bool
replay_coalesce(DETokenizer& c)
{
    fid_t srcFid, dstFid;
    size_t count;
    int64_t mtime;

    c.pop_front();
    bool ok = pop_fid(srcFid, "old", c, true);
    ok = pop_fid(dstFid, "new", c, ok);
    ok = pop_size(count, "count", c, ok);
    const bool gottime = pop_time(mtime, "mtime", c, ok);
    fid_t      retSrcFid      = -1;
    fid_t      retDstFid      = -1;
    chunkOff_t dstStartOffset = -1;
    size_t     numChunksMoved = 0;
    ok = ok && metatree.coalesceBlocks(
        metatree.getFattr(srcFid), metatree.getFattr(dstFid),
        retSrcFid, retDstFid, dstStartOffset,
        gottime ? mtime : ReplayState::get(c).mLogSegmentTimeUsec,
        numChunksMoved,
        kKfsUserRoot, kKfsGroupRoot) == 0;
    return (
        ok &&
        retSrcFid == srcFid && retDstFid == dstFid &&
        numChunksMoved == count
    );
}


/*!
 * \brief replay truncate
 * format: truncate/file/<fileID>/offset/<offset>{/mtime/<time>}
 */
static bool
replay_truncate(DETokenizer& c)
{
    fid_t fid;
    chunkOff_t offset;
    chunkOff_t endOffset;
    int status = 0;
    int64_t mtime;

    c.pop_front();
    bool ok = pop_fid(fid, "file", c, true);
    ok = pop_offset(offset, "offset", c, ok);
    // if the log has the mtime, pass it thru
    const bool gottime = pop_time(mtime, "mtime", c, ok);
    if (! gottime || ! pop_offset(endOffset, "endoff", c, ok)) {
        endOffset = -1;
    }
    if (ok) {
        const bool kSetEofHintFlag = true;
        status = metatree.truncate(fid, offset,
            gottime ? mtime : ReplayState::get(c).mLogSegmentTimeUsec,
            kKfsUserRoot, kKfsGroupRoot, endOffset, kSetEofHintFlag);
    }
    return (ok && status == 0);
}

/*!
 * \brief replay prune blks from head of file
 * format: pruneFromHead/file/<fileID>/offset/<offset>{/mtime/<time>}
 */
static bool
replay_pruneFromHead(DETokenizer& c)
{
    fid_t fid;
    chunkOff_t offset;
    int status = 0;
    int64_t mtime;

    c.pop_front();
    bool ok = pop_fid(fid, "file", c, true);
    ok = pop_fid(offset, "offset", c, ok);
    // if the log has the mtime, pass it thru
    bool gottime = pop_time(mtime, "mtime", c, ok);
    if (ok) {
        status = metatree.pruneFromHead(fid, offset,
            gottime ? mtime : ReplayState::get(c).mLogSegmentTimeUsec);
    }
    return (ok && status == 0);
}

/*!
 * \brief replay size
 * format: size/file/<fileID>/filesize/<filesize>
 */
static bool
replay_size(DETokenizer& c)
{
    fid_t fid;
    chunkOff_t filesize;

    c.pop_front();
    bool ok = pop_fid(fid, "file", c, true);
    ok = pop_offset(filesize, "filesize", c, ok);
    if (ok) {
        MetaFattr* const fa = metatree.getFattr(fid);
        if (fa) {
            if (filesize >= 0) {
                metatree.setFileSize(fa, filesize);
            } else {
                metatree.setFileSize(fa, 0);
                metatree.invalidateFileSize(fa);
            }
        }
    }
    return true;
}

/*!
 * Replay a change file replication RPC.
 * format: setrep/file/<fid>/replicas/<#>
 */

static bool
replay_setrep(DETokenizer& c)
{
    c.pop_front();
    fid_t fid;
    bool ok = pop_fid(fid, "file", c, true);
    int16_t numReplicas;
    ok = pop_short(numReplicas, "replicas", c, ok);
    kfsSTier_t minSTier = kKfsSTierUndef;
    kfsSTier_t maxSTier = kKfsSTierUndef;
    if (! c.empty()) {
        int64_t k;
        if (! pop_num(k, "minTier", c, ok)) {
            return false;
        }
        minSTier = (kfsSTier_t)k;
        if (! pop_num(k, "maxTier", c, ok)) {
            return false;
        }
        maxSTier = (kfsSTier_t)k;
    }
    if (! ok) {
        return ok;
    }
    MetaFattr* const fa = metatree.getFattr(fid);
    return (fa && metatree.changeFileReplication(
        fa, numReplicas, minSTier, maxSTier) == 0);
}

/*!
 * \brief replay setmtime
 * format: setmtime/file/<fileID>/mtime/<time>
 */
static bool
replay_setmtime(DETokenizer& c)
{
    fid_t fid;
    int64_t mtime;

    c.pop_front();
    bool ok = pop_fid(fid, "file", c, true);
    ok = pop_time(mtime, "mtime", c, ok);
    if (ok) {
        MetaFattr *fa = metatree.getFattr(fid);
        // If the fa isn't there that isn't fatal.
        if (fa != NULL)
            fa->mtime = mtime;
    }
    return ok;
}

/*!
 * \brief restore time
 * format: time/<time>
 */
static bool
replay_time(DETokenizer& c)
{
    c.pop_front();
    if (c.empty()) {
        return false;
    }
    // 2016-02-06T04:11:44.429777Z
    const char* ptr    = c.front().ptr;
    int         year   = 0;
    int         mon    = 0;
    int         mday   = 0;
    int         hour   = 0;
    int         minute = 0;
    int         sec    = 0;
    int64_t     usec   = 0;
    if (27 == c.front().len &&
            DecIntParser::Parse(ptr, 4, year) &&
            '-' == *ptr &&
            DecIntParser::Parse(++ptr, 2, mon) &&
            '-' == *ptr &&
            1 <= mon && mon <= 12 &&
            DecIntParser::Parse(++ptr, 2, mday) &&
            1 <= mday && mday <= 31 &&
            'T' == *ptr &&
            DecIntParser::Parse(++ptr, 2, hour) &&
            0 <= hour && hour <= 23 &&
            ':' == *ptr &&
            DecIntParser::Parse(++ptr, 2, minute) &&
            0 <= minute && minute <= 59 &&
            ':' == *ptr &&
            DecIntParser::Parse(++ptr, 2, sec) &&
            0 <= sec && sec <= 59 &&
            '.' == *ptr &&
            DecIntParser::Parse(++ptr, 6, usec) &&
            0 <= usec && usec <= 999999 &&
            'Z' == *ptr) {
        ReplayState::get(c).mLogSegmentTimeUsec =
            ToUnixTime(year, mon, mday, hour, minute, sec) * 1000000 + usec;
    } else {
        ReplayState::get(c).mLogSegmentTimeUsec = microseconds();
    }
    KFS_LOG_STREAM_INFO << "log time: " << c.front() << KFS_LOG_EOM;
    ReplayState::get(c).mRestoreTimeCount++;
    return true;
}

/*!
 * \brief restore make chunk stable
 * format:
 * "mkstable{done}/fileId/" << fid <<
 * "/chunkId/"        << chunkId <<
 * "/chunkVersion/"   << chunkVersion  <<
 * "/size/"           << chunkSize <<
 * "/checksum/"       << chunkChecksum <<
 * "/hasChecksum/"    << (hasChunkChecksum ? 1 : 0)
 */
static bool
replay_makechunkstable(DETokenizer& c, bool addFlag)
{
    fid_t      fid;
    chunkId_t  chunkId;
    seq_t      chunkVersion;
    chunkOff_t chunkSize;
    string     str;
    fid_t      tmp;
    uint32_t   checksum;
    bool       hasChecksum;

    c.pop_front();
    bool ok = pop_fid(fid, "fileId", c, true);
    ok = pop_fid(chunkId, "chunkId", c, ok);
    ok = pop_fid(chunkVersion, "chunkVersion", c, ok);
    int64_t num = -1;
    ok = pop_num(num, "size", c, ok);
    chunkSize = chunkOff_t(num);
    ok = pop_fid(tmp, "checksum", c, ok);
    checksum = (uint32_t)tmp;
    ok = pop_fid(tmp, "hasChecksum", c, ok);
    hasChecksum = tmp != 0;
    if (!ok) {
        KFS_LOG_STREAM_ERROR << "ignore log line for mkstable <"
            << fid << ',' << chunkId << ',' << chunkVersion
            << ">" <<
        KFS_LOG_EOM;
        return true;
    }
    if (ok) {
        gLayoutManager.ReplayPendingMakeStable(
            chunkId, chunkVersion, chunkSize,
            hasChecksum, checksum, addFlag);
    }
    return ok;
}

static bool
replay_mkstable(DETokenizer& c)
{
    return replay_makechunkstable(c, true);
}

static bool
replay_mkstabledone(DETokenizer& c)
{
    return replay_makechunkstable(c, false);
}


static bool
replay_beginchunkversionchange(DETokenizer& c)
{
    fid_t     fid;
    chunkId_t chunkId;
    seq_t     chunkVersion;

    c.pop_front();
    bool ok = pop_fid(fid,          "file",         c, true);
    ok = pop_fid     (chunkId,      "chunkId",      c, ok);
    ok = pop_fid     (chunkVersion, "chunkVersion", c, ok);
    if (! ok) {
        return false;
    }
    const bool    kPanicOnInvalidVersionFlag = false;
    string* const kStatusMsg                 = 0;
    const int ret = gLayoutManager.ProcessBeginChangeChunkVersion(
        fid, chunkId, chunkVersion, kStatusMsg, kPanicOnInvalidVersionFlag);
    return (0 == ret || -ENOENT == ret);
}

/*
  Roll file id and chunk id seeds after meta data loss. This entry must be the
  last entry in the current log segment, the log segment that was open for
  append. If no such segment exists it can be created by copying the first 4
  lines of any other log segment.
  Usually the log segments have the following entry:
  setintbase/16
  therefore the number should be in hex, the default -- 0 should be reasonable
  for the most occasions.
  Example:
  [460]mtv1% ls -ltr kfslog/ | tail -n 2
  -rw-r--r-- 2 mike users     227 Jan 22 22:11 last
  -rw-r--r-- 1 mike users      88 Jan 25 16:52 log.660
  [461]mtv1% tail kfslog/log.660
  version/1
  checksum/last-line
  setintbase/16
  time/2012-01-23T06:11:03.683655Z
  [462]mtv1% echo rollseeds/0 >> kfslog/log.660
*/
static bool
replay_rollseeds(DETokenizer& c)
{
    c.pop_front();
    if (c.empty()) {
        return false;
    }
    int64_t roll = c.toNumber();
    if (roll == 0) {
        roll = 2000000; // Default 2M
    }
    if (roll < 0 ||
            chunkID.getseed() + roll < chunkID.getseed() ||
            fileID.getseed() + roll < fileID.getseed()) {
        KFS_LOG_STREAM_ERROR <<
            "invalid seed roll value: " << roll <<
        KFS_LOG_EOM;
        return false;
    }
    chunkID.setseed(chunkID.getseed() + roll);
    fileID.setseed(fileID.getseed() + roll);
    ReplayState::get(c).mReplayer->setRollSeeds(roll);
    return true;
}

static bool
replay_chmod(DETokenizer& c)
{
    c.pop_front();
    if (c.empty()) {
        return false;
    }
    fid_t fid = -1;
    if (! pop_fid(fid, "file", c, true)) {
        return false;
    }
    int64_t n = 0;
    if (! pop_num(n, "mode", c, true)){
        return false;
    }
    const kfsMode_t mode = (kfsMode_t)n;
    MetaFattr* const fa = metatree.getFattr(fid);
    if (! fa) {
        return false;
    }
    fa->mode = mode;
    return true;
}

static bool
replay_chown(DETokenizer& c)
{
    c.pop_front();
    if (c.empty()) {
        return false;
    }
    fid_t fid = -1;
    if (! pop_fid(fid, "file", c, true)) {
        return false;
    }
    int64_t n = kKfsUserNone;
    if (! pop_num(n, "user", c, true)) {
        return false;
    }
    const kfsUid_t user = (kfsUid_t)n;
    n = kKfsGroupNone;
    if (! pop_num(n, "group", c, true)) {
        return false;
    }
    const kfsGid_t group = (kfsGid_t)n;
    if (user == kKfsUserNone && group == kKfsGroupNone) {
        return false;
    }
    MetaFattr* const fa = metatree.getFattr(fid);
    if (! fa) {
        return false;
    }
    if (user != kKfsUserNone) {
        fa->user = user;
    }
    if (group != kKfsGroupNone) {
        fa->user = group;
    }
    return true;
}

static bool
replay_inc_seq(DETokenizer& c)
{
    return ReplayState::get(c).incSeq();
}

static bool
replay_sub_entry(DETokenizer& c)
{
    return ReplayState::get(c).subEntry();
}

bool
ReplayState::runCommitQueue(
    const MetaVrLogSeq& logSeq,
    seq_t               seed,
    int64_t             status,
    int64_t             errChecksum)
{
    if (logSeq <= mCheckpointCommitted) {
        if (logSeq == mCheckpointCommitted) {
            if (errChecksum == mCheckpointErrChksum) {
                return true;
            }
            KFS_LOG_STREAM_ERROR <<
                "commit"
                " sequence: "       << logSeq <<
                " checkpoint: "     << mCheckpointCommitted <<
                " error checksum: " << errChecksum <<
                " expected: "       << mCheckpointErrChksum <<
            KFS_LOG_EOM;
            return false;
        }
        return true;
    }
    CommitQueue::iterator it = mCommitQueue.begin();
    while (mCommitQueue.end() != it) {
        CommitQueueEntry& f = *it;
        if (logSeq < f.logSeq) {
            break;
        }
        commit(f);
        if (logSeq == f.logSeq) {
            if (f.status == status && f.seed == seed &&
                    f.errChecksum == errChecksum) {
                mCommitQueue.erase(mCommitQueue.begin(), it);
                return true;
            }
            for (CommitQueue::const_iterator cit = mCommitQueue.begin();
                    ; ++cit) {
                KFS_LOG_STREAM_ERROR <<
                    "commit"
                    " sequence: "       << cit->logSeq <<
                    " "                 << hex << cit->logSeq << dec <<
                    " seed: "           << cit->seed <<
                    " error checksum: " << cit->errChecksum <<
                    " status: "         << cit->status <<
                    " [" << (cit->status == 0 ? string("OK") :
                        ErrorCodeToString(-KfsToSysErrno(cit->status))) <<
                    "]" <<
                KFS_LOG_EOM;
                if (cit == it) {
                    break;
                }
            }
            KFS_LOG_STREAM_ERROR <<
                "log commit:"
                " sequence: " << logSeq <<
                " "           << hex << logSeq << dec <<
                " status mismatch"
                " expected: " << status <<
                " [" << ErrorCodeToString(-KfsToSysErrno(status)) << "]"
                " actual: "   << f.status <<
                " [" << ErrorCodeToString(-KfsToSysErrno(f.status)) << "]" <<
                " seed:"
                " expected: " << f.seed <<
                " actual: "   << seed <<
                " error checksum:"
                " expected: " << f.errChecksum <<
                " actual: "   << errChecksum <<
            KFS_LOG_EOM;
            return false;
        }
        ++it;
    }
    // Commit sequence must always be at the log block end.
    if (it != mCommitQueue.begin() ||
            fileID.getseed() != seed ||
            mLastCommittedStatus != status ||
            mLogAheadErrChksum != errChecksum) {
        KFS_LOG_STREAM_ERROR <<
            "invliad log commit:"
            " sequence: "       << logSeq <<
            " / "               << (mCommitQueue.empty() ?
                MetaVrLogSeq() : mCommitQueue.front().logSeq) <<
            " status: "         << status <<
            " [" << ErrorCodeToString(-KfsToSysErrno(status)) << "]" <<
            " expected: "       << mLastCommittedStatus <<
            " [" << ErrorCodeToString(
                    -KfsToSysErrno(mLastCommittedStatus)) << "]" <<
            " seed: "           << seed <<
            " expected: "       << fileID.getseed() <<
            " error checksum: " << errChecksum <<
            " expected: "       << mLogAheadErrChksum <<
            " commit queue: "   << mCommitQueue.size() <<
        KFS_LOG_EOM;
    }
    return false;
}

static bool
replay_log_ahead_entry(DETokenizer& c)
{
    ReplayState& state = ReplayState::get(c);
    if (0 != state.mSubEntryCount || state.mCurOp) {
        KFS_LOG_STREAM_ERROR <<
            "invalid replay state:"
            " sub entry count: " << state.mSubEntryCount <<
            " cur op: "          << MetaRequest::ShowReq(state.mCurOp) <<
        KFS_LOG_EOM;
        return false;
    }
    c.pop_front();
    if (c.empty()) {
        return false;
    }
    const DETokenizer::Token& token = c.front();
    state.mCurOp = MetaRequest::ReadReplay(token.ptr, token.len);
    if (! state.mCurOp) {
        KFS_LOG_STREAM_ERROR <<
            "replay parse failure:"
            " logseq: " << state.mLastLogAheadSeq <<
            " "         << token <<
        KFS_LOG_EOM;
        return false;
    }
    if (! state.IsCurOpLogSeqValid()) {
        return false;
    }
    state.mCurOp->replayFlag = true;
    state.handle();
    return state.incSeq();
}

static bool
replay_log_commit_entry(DETokenizer& c, Replay::BlockChecksum& blockChecksum)
{
    if (c.size() < 9) {
        return false;
    }
    const char* const ptr   = c.front().ptr;
    const size_t      len   = c.back().ptr - ptr;
    const size_t      skip  = len + c.back().len;
    ReplayState&      state = ReplayState::get(c);
    blockChecksum.write(ptr, len);
    c.pop_front();
    MetaVrLogSeq commitSeq;
    if (! parse_vr_log_seq(c, commitSeq) || ! commitSeq.IsValid()) {
        return false;
    }
    c.pop_front();
    const int64_t seed = c.toNumber();
    if (! c.isLastOk()) {
        return false;
    }
    c.pop_front();
    const int64_t errchksum = c.toNumber();
    if (! c.isLastOk()) {
        return false;
    }
    c.pop_front();
    const int64_t status = c.toNumber();
    if (! c.isLastOk() || status < 0) {
        return false;
    }
    c.pop_front();
    MetaVrLogSeq logSeq;
    if (! parse_vr_log_seq(c, logSeq) || logSeq != state.mLastLogAheadSeq) {
        return false;
    }
    c.pop_front();
    const int64_t blockLen = c.toNumber();
    if (! c.isLastOk() || blockLen < 0 ||
            state.mBlockStartLogSeq.mLogSeq + blockLen != logSeq.mLogSeq) {
        return false;
    }
    c.pop_front();
    const int64_t blockSeq = c.toNumber();
    if (! c.isLastOk() || blockSeq != state.mLastBlockSeq + 1) {
        return false;
    }
    c.pop_front();
    const int64_t checksum = c.toNumber();
    if (! c.isLastOk() || checksum < 0) {
        return false;
    }
    const uint32_t expectedChecksum = blockChecksum.blockEnd(skip);
    if ((int64_t)expectedChecksum != checksum) {
        KFS_LOG_STREAM_ERROR <<
            "record block checksum mismatch:"
            " expected: " << expectedChecksum <<
            " actual: "   << checksum <<
        KFS_LOG_EOM;
        return false;
    }
    if (commitSeq < state.mLastCommitted ||
            state.mLastLogAheadSeq < commitSeq) {
        KFS_LOG_STREAM_ERROR <<
            "committed:"
            " expected range: [" << state.mLastCommitted <<
            ","                  << state.mLastLogAheadSeq << "]"
            " actual: "          << commitSeq <<
        KFS_LOG_EOM;
        return false;
    }
    if (! state.runCommitQueue(commitSeq, seed, status, errchksum)) {
        return false;
    }
    if (0 != state.mSubEntryCount) {
        return false;
    }
    state.mBlockStartLogSeq = logSeq;
    state.mLastBlockSeq     = blockSeq;
    state.mLastCommitted    = commitSeq;
    return true;
}

static bool
replay_group_users_reset(DETokenizer& c)
{
    if (! restore_group_users_reset(c) || c.empty()) {
        return false;
    }
    ReplayState& state = ReplayState::get(c);
    state.mSubEntryCount = c.toNumber();
    return (0 <= state.mSubEntryCount && c.isLastOk());
}

static bool
replay_setfsinfo(DETokenizer& c)
{
    return (restore_filesystem_info(c) && replay_inc_seq(c));
}

static bool
replay_group_users(DETokenizer& c)
{
    return (restore_group_users(c) && replay_sub_entry(c));
}

static bool
replay_clear_obj_store_delete(DETokenizer& c)
{
    c.pop_front();
    gLayoutManager.ClearObjStoreDelete();
    return true;
}

static bool
replay_cs_hello(DETokenizer& c)
{
    const DETokenizer::Token& verb  = c.front();
    ReplayState&              state = ReplayState::get(c);
    MetaHello*                op;
    if (3 == verb.len) {
        if (0 != state.mSubEntryCount) {
            return false;
        }
        c.pop_front();
        int64_t n;
        if (! pop_num(n, "e", c, true) || n < 0) {
            return false;
        }
        if ("l" != c.front()) {
            return false;
        }
        c.pop_front();
        state.mSubEntryCount = n;
        op = new MetaHello();
        state.mCurOp = op;
        op->replayFlag = true;
        const DETokenizer::Token& loc = c.front();
        if (! op->location.FromString(loc.ptr, loc.len, 16 == c.getIntBase())) {
            return false;
        }
        c.pop_front();
        if (! pop_num(n, "s", c, true) || n < 0) {
            return false;
        }
        op->numChunks = (int)n;
        if (! pop_num(n, "n", c, true) || n < 0) {
            return false;
        }
        op->numNotStableChunks = (int)n;
        if (! pop_num(n, "a", c, true) || n < 0) {
            return false;
        }
        op->numNotStableAppendChunks = (int)n;
        if (! pop_num(n, "m", c, true) || n < 0) {
            return false;
        }
        op->numMissingChunks = (int)n;
        if (pop_num(n, "p", c, true)) {
            if (n < 0) {
                return false;
            }
            op->numPendingStaleChunks = (int)n;
        } else {
            op->numPendingStaleChunks = 0;
        }
        if (! pop_num(n, "d", c, true) || n < 0) {
            return false;
        }
        op->deletedCount = (size_t)n;
        if (! pop_num(n, "r", c, true)) {
            return false;
        }
        op->resumeStep = (int)n;
        if (! pop_num(n, "t", c, true)) {
            return false;
        }
        op->timeUsec = n;
        if (! pop_num(n, "r", c, true)) {
            return false;
        }
        op->rackId = (int)n;
        if (! pop_num(n, "P", c, true)) {
            return false;
        }
        op->pendingNotifyFlag = 0 != n;
        if (1 != c.front().len || 'z' != c.front().ptr[0]) {
            return false;
        }
        c.pop_front();
        if (! parse_vr_log_seq(c, op->logseq) || ! op->logseq.IsValid()) {
            return false;
        }
        c.pop_front();
        if (state.mReplayer) {
            MetaVrLogSeq next = state.mLastLogAheadSeq;
            next.mLogSeq++;
            if (op->logseq != next) {
                return false;
            }
        }
        op->chunks.reserve(op->numChunks);
        op->notStableChunks.reserve(op->numNotStableChunks);
        op->notStableAppendChunks.reserve(op->numNotStableAppendChunks);
        op->missingChunks.reserve(op->numMissingChunks);
    } else {
        if (4 != verb.len || ! state.mCurOp) {
            return false;
        }
        op = static_cast<MetaHello*>(state.mCurOp);
        if ('c' == verb.ptr[3]) {
            c.pop_front();
            if (c.empty()) {
                return false;
            }
            MetaHello::ChunkInfo info;
            while (! c.empty()) {
                info.chunkId = c.toNumber();
                if (! c.isLastOk() || info.chunkId < 0) {
                    return false;
                }
                c.pop_front();
                if (c.empty()) {
                    return false;
                }
                info.chunkVersion = c.toNumber();
                if (! c.isLastOk() || info.chunkVersion < 0) {
                    return false;
                }
                c.pop_front();
                if (op->chunks.size() < (size_t)op->numChunks) {
                    op->chunks.push_back(info);
                } else if (op->notStableChunks.size() <
                        (size_t)op->numNotStableChunks) {
                    op->notStableChunks.push_back(info);
                } else if (op->notStableAppendChunks.size() <
                        (size_t)op->numNotStableAppendChunks) {
                    op->notStableAppendChunks.push_back(info);
                } else {
                    return false;
                }
            }
        } else {
            const int type = verb.ptr[3] & 0xFF;
            if ('m' != type && 'p' != type) {
                return false;
            }
            c.pop_front();
            if (c.empty()) {
                return false;
            }
            MetaHello::ChunkIdList& list = 'm' == type ?
                op->missingChunks : op->pendingStaleChunks;
            const size_t            cnt  = max(0, 'm' == type ?
                op->numMissingChunks : op->numPendingStaleChunks);
            while (! c.empty()) {
                const int64_t n = c.toNumber();
                if (! c.isLastOk() || n < 0) {
                    return false;
                }
                c.pop_front();
                if (cnt <= list.size()) {
                    return false;
                }
                list.push_back(n);
            }
        }
    }
    if (1 == state.mSubEntryCount) {
        if (op->chunks.size() != (size_t)op->numChunks ||
                op->notStableChunks.size() != (size_t)op->numNotStableChunks ||
                op->notStableAppendChunks.size() !=
                    (size_t)op->numNotStableAppendChunks ||
                op->missingChunks.size() != (size_t)op->numMissingChunks ||
                op->pendingStaleChunks.size() !=
                    (size_t)op->numPendingStaleChunks) {
            return false;
        }
        state.replayCurOp();
    }
    return replay_sub_entry(c);
}

static bool
replay_cs_inflight(DETokenizer& c)
{
    const DETokenizer::Token& verb  = c.front();
    ReplayState&              state = ReplayState::get(c);
    MetaChunkLogInFlight*     op;
    if (3 == verb.len && 's' == verb.ptr[2]) {
        op = static_cast<MetaChunkLogInFlight*>(state.mCurOp);
        if (! op) {
            return false;
        }
        c.pop_front();
        while (! c.empty()) {
            const int64_t n = c.toNumber();
            if (! c.isLastOk() || n < 0) {
                return false;
            }
            c.pop_front();
            if ((size_t)op->idCount <= op->chunkIds.GetSize()) {
                return false;
            }
            op->chunkIds.PushBack(n);
        }
        if (1 == state.mSubEntryCount &&
                (size_t)op->idCount != op->chunkIds.GetSize()) {
            return false;
        }
    } else {
        if (0 != state.mSubEntryCount || state.mCurOp) {
            return false;
        }
        c.pop_front();
        int64_t n;
        if (! pop_num(n, "e", c, true) || n < 0) {
            return false;
        }
        if ("l" != c.front()) {
            return false;
        }
        c.pop_front();
        state.mSubEntryCount = n;
        op = new MetaChunkLogInFlight();
        state.mCurOp = op;
        op->replayFlag = true;
        const DETokenizer::Token& loc = c.front();
        if (! op->location.FromString(loc.ptr, loc.len, 16 == c.getIntBase())) {
            return false;
        }
        c.pop_front();
        if (! pop_num(n, "s", c, true) || n < 0) {
            return false;
        }
        op->idCount = n;
        if (! pop_num(n, "c", c, true) || (0 < op->idCount && 0 <= n)) {
            return false;
        }
        op->chunkId = n;
        if (! pop_num(n, "x", c, true) || n < 0) {
            return false;
        }
        op->removeServerFlag = 0 != n;
        if ("r" != c.front()) {
            return false;
        }
        c.pop_front();
        if (c.empty()) {
            return false;
        }
        // Original request type, presently used for debugging.
        const DETokenizer::Token& rtype = c.front();
        op->reqType = MetaChunkLogInFlight::GetReqId(rtype.ptr, rtype.len);
        c.pop_front();
        if (1 != c.front().len || 'z' != c.front().ptr[0]) {
            return false;
        }
        c.pop_front();
        if (! parse_vr_log_seq(c, op->logseq) || ! op->logseq.IsValid()) {
            return false;
        }
        c.pop_front();
        if (state.mReplayer) {
            MetaVrLogSeq next = state.mLastLogAheadSeq;
            next.mLogSeq++;
            if (next != op->logseq) {
                return false;
            }
        }
    }
    if (1 == state.mSubEntryCount) {
        state.replayCurOp();
    }
    return replay_sub_entry(c);
}

bool
restore_chunk_server_end(DETokenizer& c)
{
    return replay_inc_seq(c);
}

static DiskEntry&
get_entry_map()
{
    static bool initied = false;
    static DiskEntry e;
    if (initied) {
        return e;
    }
    e.add_parser("setintbase",              &restore_setintbase);
    e.add_parser("version",                 &replay_version);
    e.add_parser("create",                  &replay_create);
    e.add_parser("mkdir",                   &replay_mkdir);
    e.add_parser("remove",                  &replay_remove);
    e.add_parser("rmdir",                   &replay_rmdir);
    e.add_parser("rename",                  &replay_rename);
    e.add_parser("allocate",                &replay_allocate);
    e.add_parser("truncate",                &replay_truncate);
    e.add_parser("coalesce",                &replay_coalesce);
    e.add_parser("pruneFromHead",           &replay_pruneFromHead);
    e.add_parser("setrep",                  &replay_setrep);
    e.add_parser("size",                    &replay_size);
    e.add_parser("setmtime",                &replay_setmtime);
    e.add_parser("chunkVersionInc",         &restore_chunkVersionInc);
    e.add_parser("time",                    &replay_time);
    e.add_parser("mkstable",                &replay_mkstable);
    e.add_parser("mkstabledone",            &replay_mkstabledone);
    e.add_parser("beginchunkversionchange", &replay_beginchunkversionchange);
    e.add_parser("checksum",                &restore_checksum);
    e.add_parser("rollseeds",               &replay_rollseeds);
    e.add_parser("chmod",                   &replay_chmod);
    e.add_parser("chown",                   &replay_chown);
    e.add_parser("delegatecancel",          &restore_delegate_cancel);
    e.add_parser("filesysteminfo",          &restore_filesystem_info);
    e.add_parser("clearobjstoredelete",     &replay_clear_obj_store_delete);
    // Write ahead log entries.
    e.add_parser("setfsinfo",               &replay_setfsinfo);
    e.add_parser("gur",                     &replay_group_users_reset);
    e.add_parser("gu",                      &replay_group_users);
    e.add_parser("guc",                     &replay_group_users);
    e.add_parser("csh",                     &replay_cs_hello);
    e.add_parser("cshc",                    &replay_cs_hello);
    e.add_parser("cshm",                    &replay_cs_hello);
    e.add_parser("cshp",                    &replay_cs_hello);
    e.add_parser("cif",                     &replay_cs_inflight);
    e.add_parser("cis",                     &replay_cs_inflight);
    initied = true;
    return e;
}

/* static */ void
Replay::AddRestotreEntries(DiskEntry& e)
{
    e.add_parser("cif", &replay_cs_inflight);
    e.add_parser("cis", &replay_cs_inflight);
}

Replay::BlockChecksum::BlockChecksum()
    : skip(0),
      checksum(kKfsNullChecksum)
{}

uint32_t
Replay::BlockChecksum::blockEnd(size_t s)
{
    skip = s;
    const uint32_t ret = checksum;
    checksum = kKfsNullChecksum;
    return ret;
}

bool
Replay::BlockChecksum::write(const char* buf, size_t len)
{
    if (len <= skip) {
        skip -= len;
    } else {
        checksum = ComputeBlockChecksum(checksum, buf + skip, len - skip);
        skip = 0;
    }
    return true;
}

Replay::Tokenizer::Tokenizer(istream& file, Replay* replay)
     : state(*(new ReplayState(replay))),
       tokenizer(*(new DETokenizer(file, &state)))
{}

Replay::Tokenizer::~Tokenizer()
{
    delete &tokenizer;
    delete &state;
}

const DETokenizer::Token kAheadLogEntry ("a", 1);
const DETokenizer::Token kCommitLogEntry("c", 1);

Replay::Replay()
    : file(),
      path(),
      number(-1),
      lastLogNum(-1),
      lastLogIntBase(-1),
      appendToLastLogFlag(false),
      verifyAllLogSegmentsPresetFlag(false),
      checkpointCommitted(),
      committed(0, 0, 0),
      lastLogStart(0, 0, 0),
      lastLogSeq(0, 0, 0),
      lastBlockSeq(-1),
      errChecksum(0),
      rollSeeds(0),
      lastCommittedStatus(0),
      tmplogprefixlen(0),
      tmplogname(),
      logdir(),
      mds(),
      replayTokenizer(file, this),
      entrymap(get_entry_map()),
      blockChecksum(),
      maxLogNum(-1),
      logSeqStartNum(-1)
    {}

Replay::~Replay()
{}

int
Replay::playLine(const char* line, int len, seq_t blockSeq)
{
    if (len <= 0) {
        return 0;
    }
    DETokenizer& tokenizer = replayTokenizer.Get();
    ReplayState& state     = replayTokenizer.GetState();
    tokenizer.setIntBase(16);
    if (0 <= blockSeq) {
        state.mLastBlockSeq = blockSeq - 1;
    }
    int status = 0;
    if (! tokenizer.next(line, len)) {
        status = -EINVAL;
    }
    if (0 == status && ! tokenizer.empty()) {
        if (! (kAheadLogEntry == tokenizer.front() ?
                replay_log_ahead_entry(tokenizer) :
                (kCommitLogEntry == tokenizer.front() ?
                    replay_log_commit_entry(tokenizer, blockChecksum) :
                    entrymap.parse(tokenizer)))) {
            KFS_LOG_STREAM_ERROR <<
                "error block seq: " << blockSeq <<
                ":" << tokenizer.getEntryCount() <<
                ":" << tokenizer.getEntry() <<
            KFS_LOG_EOM;
            status = -EINVAL;
        }
    }
    if (0 == status) {
        lastCommittedStatus = state.mLastCommittedStatus;
    }
    if (0 <= blockSeq || 0 != status) {
        blockChecksum.blockEnd(0);
        blockChecksum.write("\n", 1);
        if (state.mSubEntryCount != 0 && 0 == status) {
            KFS_LOG_STREAM_ERROR <<
                "invalid block commit:"
                " sub entry count: " << state.mSubEntryCount <<
            KFS_LOG_EOM;
            state.mSubEntryCount = 0;
            status = -EINVAL;
            // Next block implicitly includes leading new line.
            tokenizer.resetEntryCount();
        }
    } else {
        blockChecksum.write(line, len);
    }
    return status;
}

/*!
 * \brief replay contents of log file
 * \return  zero if replay successful, negative otherwise
 */
int
Replay::playlog(bool& lastEntryChecksumFlag)
{
    restoreChecksum.clear();
    lastLineChecksumFlag = false;
    lastEntryChecksumFlag = false;
    blockChecksum.blockEnd(0);
    mds.Reset(&blockChecksum);
    mds.SetWriteTrough(true);

    if (! file.is_open()) {
        //!< no log...so, reset the # to 0.
        number = 0;
        return 0;
    }

    ReplayState& state       = replayTokenizer.GetState();
    lastLogStart             = state.mLastLogAheadSeq;
    state.mLastBlockSeq      = -1;
    state.mSubEntryCount     = 0;
    state.mLogAheadErrChksum = errChecksum;
    int          status    = 0;
    DETokenizer& tokenizer = replayTokenizer.Get();
    tokenizer.reset();
    while (tokenizer.next(&mds)) {
        if (tokenizer.empty()) {
            continue;
        }
        if (! (kAheadLogEntry == tokenizer.front() ?
                replay_log_ahead_entry(tokenizer) :
                (kCommitLogEntry == tokenizer.front() ?
                    replay_log_commit_entry(tokenizer, blockChecksum) :
                    entrymap.parse(tokenizer)))) {
            KFS_LOG_STREAM_FATAL <<
                "error " << path <<
                ":" << tokenizer.getEntryCount() <<
                ":" << tokenizer.getEntry() <<
            KFS_LOG_EOM;
            status = -EINVAL;
            break;
        }
        lastEntryChecksumFlag = ! restoreChecksum.empty();
        if (lastEntryChecksumFlag) {
            const string md = mds.GetMd();
            if (md != restoreChecksum) {
                KFS_LOG_STREAM_FATAL <<
                    "error " << path <<
                    ":" << tokenizer.getEntryCount() <<
                    ":" << tokenizer.getEntry() <<
                    ": checksum mismatch:"
                    " expectd:" << restoreChecksum <<
                    " computed: " << md <<
                KFS_LOG_EOM;
                status = -EINVAL;
                break;
            }
            restoreChecksum.clear();
        }
    }
    if (status == 0 && 0 != state.mSubEntryCount) {
        KFS_LOG_STREAM_FATAL <<
            "error " << path <<
            " invalid sub entry count: " << state.mSubEntryCount <<
        KFS_LOG_EOM;
        status = -EIO;
    }
    if (status == 0 && ! file.eof()) {
        KFS_LOG_STREAM_FATAL <<
            "error " << path <<
            ":" << tokenizer.getEntryCount() <<
            ":" << tokenizer.getEntry() <<
        KFS_LOG_EOM;
        status = -EIO;
    }
    if (status == 0) {
        errChecksum    = state.mLogAheadErrChksum;
        lastLogIntBase = tokenizer.getIntBase();
        lastBlockSeq   = state.mLastBlockSeq;
        mds.SetStream(0);
    }
    file.close();
    blockChecksum.blockEnd(0);
    blockChecksum.write("\n", 1);
    tokenizer.resetEntryCount();
    return status;
}

/*!
 * \brief replay contents of all log files since CP
 * \return  zero if replay successful, negative otherwise
 */
int
Replay::playLogs(bool includeLastLogFlag)
{
    if (number < 0) {
        //!< no log...so, reset the # to 0.
        number = 0;
        appendToLastLogFlag = false;
        return 0;
    }
    ReplayState& state = replayTokenizer.GetState();
    // Log commit can be less than checkpoint.
    state.mLastCommitted       = MetaVrLogSeq();
    state.mCheckpointCommitted = committed;
    state.mCheckpointErrChksum = errChecksum;
    const int status = getLastLogNum();
    return (status == 0 ?
        playLogs(lastLogNum, includeLastLogFlag) : status);
}

int
Replay::playLogs(seq_t last, bool includeLastLogFlag)
{
    appendToLastLogFlag        = false;
    lastLineChecksumFlag       = false;
    lastLogIntBase             = -1;
    bool lastEntryChecksumFlag = false;
    bool         completeSegmentFlag = true;
    int          status              = 0;
    ReplayState& state               = replayTokenizer.GetState();
    state.mLastLogAheadSeq   = committed;
    state.mBlockStartLogSeq  = committed;
    for (seq_t i = number; ; i++) {
        if (! includeLastLogFlag && last < i) {
            break;
        }
        // Check if the next log segment exists prior to loading current log
        // segment in order to allow fsck to load all segments while meta server
        // is running. The meta server might close the current segment, and
        // create the new segment after reading / loading tail of the current
        // segment, in which case the last read might not have the last checksum
        // line.
        if (last < i && maxLogNum <= i) {
            completeSegmentFlag = ! logSegmentHasLogSeq(i + 1) &&
                file_exists(logfile(i + 1));
            if (! completeSegmentFlag && maxLogNum < i &&
                    ! file_exists(logfile(i))) {
                break;
            }
        }
        state.mRestoreTimeCount = 0;
        const string logfn = logfile(i);
        if ((status = openlog(logfn)) != 0 ||
                (status = playlog(lastEntryChecksumFlag)) != 0) {
            break;
        }
        if (state.mRestoreTimeCount <= 0) {
            // "time/" is the last line of the header.
            // Each valid log even last partial must have
            // complete header.
            KFS_LOG_STREAM_FATAL <<
                logfn <<
                ": missing \"time\" line" <<
            KFS_LOG_EOM;
            status = -EINVAL;
            break;
        }
        if (lastLineChecksumFlag &&
                (! lastEntryChecksumFlag && completeSegmentFlag)) {
            KFS_LOG_STREAM_FATAL <<
                logfn <<
                ": missing last line checksum" <<
            KFS_LOG_EOM;
            status = -EINVAL;
            break;
        }
        number = i;
        if (last < i && ! lastEntryChecksumFlag) {
            appendToLastLogFlag = true;
            break;
        }
    }
    if (status == 0 &&
            MetaRequest::GetLogWriter().GetMetaVrSM().GetConfig().IsEmpty()) {
        status = state.commmitAll() ? 0 : -EINVAL;
    }
    if (status == 0) {
        lastLogSeq          = state.mLastLogAheadSeq;
        committed           = state.mLastCommitted;
        errChecksum         = state.mLogAheadErrChksum;
        lastCommittedStatus = state.mLastCommittedStatus;
        // For now update checkpont committed in order to make replay line
        // work at startup with all requests already committed.
        state.mCheckpointCommitted = state.mLastCommitted;
        state.mCheckpointErrChksum = state.mLogAheadErrChksum;
    } else {
        appendToLastLogFlag = false;
    }
    return status;
}

static int
ValidateLogSegmentTrailer(
    const char* name,
    bool        completeSegmentFlag)
{
    int                       ret       = 0;
    ifstream::streamoff const kTailSize = 1 << 10;
    ifstream::streamoff       pos       = -1;
    ifstream fs(name, ifstream::in | ifstream::binary);
    if (fs && fs.seekg(0, ifstream::end) && 0 <= (pos = fs.tellg())) {
        ifstream::streamoff sz;
        if (kTailSize < pos) {
            sz = kTailSize;
            pos -= kTailSize;
        } else {
            sz  = pos;
            pos = 0;
        }
        StBufferT<char, 1> buf;
        char* const        ptr = buf.Resize(sz + ifstream::streamoff(1));
        if (fs.seekg(pos, ifstream::beg) && fs.read(ptr, sz)) {
            if (sz != fs.gcount()) {
                KFS_LOG_STREAM_FATAL <<
                    name << ": "
                    "invalid read size:"
                    " actual: "   << fs.gcount() <<
                    " expected: " << sz <<
                KFS_LOG_EOM;
                ret = -EIO;
            } else {
                ptr[sz] = 0;
                const char*       p = ptr + sz - 1;
                const char* const b = ptr;
                if (p <= b || '\n' != *p) {
                    ret = -EINVAL;
                    KFS_LOG_STREAM_FATAL <<
                        name << ": no trailing new line: " <<
                        ptr <<
                    KFS_LOG_EOM;
                } else {
                    // Last line must start with log block trailer
                    // if segment is not complete / closed or checksum line
                    // otherwise.
                    --p;
                    while (b < p && '\n' != *p) {
                        --p;
                    }
                    if (p <= b ||
                            ((completeSegmentFlag || ptr + sz < p + 3 ||
                                    0 != memcmp("c/", p + 1, 2)) &&
                            (ptr + sz < p + 10 ||
                                0 != memcmp("checksum/", p + 1, 9)))) {
                        KFS_LOG_STREAM_FATAL <<
                            name << ": invalid log segment trailer: " <<
                            (p + 1) <<
                        KFS_LOG_EOM;
                        ret = -EINVAL;
                    }
                }
            }
        }
    }
    if (0 == ret && (! fs || pos < 0)) {
        const int err = errno;
        KFS_LOG_STREAM_FATAL <<
            name << ": " << QCUtils::SysError(err) <<
        KFS_LOG_EOM;
        ret = 0 < err ? -err : (err == 0 ? -EIO : err);
    }
    fs.close();
    return ret;
}

typedef map<
    seq_t,
    string,
    less<seq_t>,
    StdFastAllocator<pair<const seq_t, string> >
> LogSegmentNumbers;

int
Replay::getLastLogNum()
{
    if (0 <= lastLogNum) {
        return 0;
    }
    lastLogNum     = number;
    maxLogNum      = -1;
    logSeqStartNum = -1;
    if (lastLogNum < 0) {
        // no logs, to replay.
        return 0;
    }
    // Get last complete log number. All log files before and including this
    // won't ever be written to again.
    // Get the inode # for the last file
    const string lastlog = getLastLog();
    struct stat lastst = {0};
    if (stat(lastlog.c_str(), &lastst)) {
        const int err = errno;
        if (ENOENT == err) {
            lastLogNum = -1; // Checkpoint with single log segment.
        } else {
            KFS_LOG_STREAM_FATAL <<
                lastlog <<
                ": " << QCUtils::SysError(err) <<
            KFS_LOG_EOM;
            return (0 < err ? -err : (err == 0 ? -EIO : err));
        }
    }
    if (0 <= lastLogNum && lastst.st_nlink != 2) {
        KFS_LOG_STREAM_FATAL <<
            lastlog <<
            ": invalid link count: " << lastst.st_nlink <<
            " this must be \"hard\" link to the last complete log"
            " segment (usually the last log segment with last line starting"
            " with \"checksum/\" prefix), and therefore must have link"
            " count 2" <<
        KFS_LOG_EOM;
        return -EINVAL;
    }
    string            dirName  = lastlog;
    string::size_type pos      = dirName.rfind('/');
    const char*       lastName = lastlog.c_str();
    if (string::npos != pos) {
        lastName += pos + 1;
        if (pos <= 0) {
            dirName = "/";
        } else {
            dirName.erase(pos);
        }
    } else {
        dirName = ".";
    }
    DIR* const dir = opendir(dirName.c_str());
    if (! dir) {
        const int err = errno;
        KFS_LOG_STREAM_FATAL <<
            dirName << ": " << QCUtils::SysError(err) <<
        KFS_LOG_EOM;
        return (err > 0 ? -err : (err == 0 ? -1 : err));
    }
    int                  ret = 0;
    LogSegmentNumbers    logNums;
    const struct dirent* ent;
    while ((ent = readdir(dir))) {
        if (strcmp(ent->d_name, lastName) == 0) {
            continue;
        }
        const char* const p   = strrchr(ent->d_name, '.');
        const int64_t     num = p ? toNumber(p + 1) : int64_t(-1);
        if (0 <= lastLogNum && lastst.st_ino == ent->d_ino) {
            lastLogNum = num;
            if (num < 0) {
                KFS_LOG_STREAM_FATAL <<
                    "invalid log segment name: " <<
                        dirName << "/" << ent->d_name <<
                KFS_LOG_EOM;
                ret = -EINVAL;
                break;
            }
        }
        if (num < 0) {
            continue;
        }
        // Find first, if any, log segment number in the form
        // log.<log sequence>.<log number>
        const char* s = p;
        while (ent->d_name <= --s) {
            const int sym = *s & 0xFF;
            if ('.' == sym) {
                if (s + 1 < p && p <= s + 22) {
                    logSeqStartNum = logSeqStartNum < 0 ? num :
                        min(num, logSeqStartNum);
                }
                break;
            }
            if (sym < '0' || '9' < sym) {
                break;
            }
        }
        if (lastLogNum < 0 && number < num) {
            KFS_LOG_STREAM_FATAL <<
                "no link to last complete log segment: " << lastlog <<
            KFS_LOG_EOM;
            ret = -EINVAL;
            break;
        }
        if ((verifyAllLogSegmentsPresetFlag || number <= num) &&
                ! logNums.insert(make_pair(num, ent->d_name)).second) {
            KFS_LOG_STREAM_FATAL <<
                "duplicate log segment number: " << num <<
                " " << dirName << "/" << ent->d_name <<
            KFS_LOG_EOM;
            ret = -EINVAL;
            break;
        }
        if (maxLogNum < num) {
            maxLogNum = num;
        }
    }
    closedir(dir);
    if (0 == ret && maxLogNum < 0) {
        KFS_LOG_STREAM_FATAL <<
            "no log segments found: " << dirName <<
        KFS_LOG_EOM;
        ret = -EINVAL;
    }
    LogSegmentNumbers::const_iterator it = logNums.begin();
    if (logNums.end() == it || (verifyAllLogSegmentsPresetFlag ?
            logNums.find(number) == logNums.end() : it->first != number)) {
        KFS_LOG_STREAM_FATAL <<
            "missing log segmnet: " << number <<
        KFS_LOG_EOM;
        ret = -EINVAL;
    } else {
        seq_t n = it->first;
        while (logNums.end() != ++it) {
            if (++n != it->first) {
                KFS_LOG_STREAM_FATAL <<
                    "missing log segmnets:"
                    " from: " << n  <<
                    " to: "   << it->first <<
                KFS_LOG_EOM;
                n = it->first;
                ret = -EINVAL;
            }
        }
    }
    if (0 == ret && 0 <= logSeqStartNum) {
        it = logNums.find(logSeqStartNum);
        string name;
        while (logNums.end() != it) {
            name = dirName + "/" + it->second;
            ++it;
            if (0 != (ret = ValidateLogSegmentTrailer(
                    name.c_str(), logNums.end() != it))) {
                break;
            }
        }
    }
    return ret;
}

void
Replay::handle(MetaVrLogStartView& op)
{
    if (0 != op.status || ! op.Validate()) {
        panic("replay: invalid log start view log entry");
        op.status = -EINVAL;
        return;
    }
    if (! op.replayFlag) {
        return;
    }
    replayTokenizer.GetState().handleStartView(op);
}

bool
Replay::setReplayState(
    const MetaVrLogSeq& committed,
    int64_t             errChecksum,
    int                 lastCommittedStatus,
    MetaRequest*        commitQueue)
{
    return replayTokenizer.GetState().setReplayState(
        committed, errChecksum, lastCommittedStatus, commitQueue);
}

} // namespace KFS
