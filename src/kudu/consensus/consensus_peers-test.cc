// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include <gtest/gtest.h>

#include "kudu/common/schema.h"
#include "kudu/common/wire_protocol-test-util.h"
#include "kudu/consensus/consensus_peers.h"
#include "kudu/consensus/consensus-test-util.h"
#include "kudu/consensus/log.h"
#include "kudu/consensus/log_anchor_registry.h"
#include "kudu/consensus/log_util.h"
#include "kudu/consensus/opid_util.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/util/metrics.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

namespace kudu {
namespace consensus {

using log::Log;
using log::LogOptions;
using log::LogAnchorRegistry;
using metadata::QuorumPeerPB;

const char* kTabletId = "test-peers-tablet";
const char* kLeaderUuid = "test-peers-leader";

class ConsensusPeersTest : public KuduTest {
 public:
  ConsensusPeersTest()
    : metric_context_(&metric_registry_, "peer-test"),
      schema_(GetSimpleTestSchema()) {
    CHECK_OK(ThreadPoolBuilder("test-peer-pool").set_max_threads(1).Build(&pool_));
  }

  virtual void SetUp() OVERRIDE {
    KuduTest::SetUp();
    fs_manager_.reset(new FsManager(env_.get(), test_dir_));
    CHECK_OK(Log::Open(options_,
                       fs_manager_.get(),
                       kTabletId,
                       schema_,
                       NULL,
                       &log_));
    consensus_.reset(new TestRaftConsensusQueueIface(log_.get()));
    message_queue_.reset(new PeerMessageQueue(metric_context_));
  }

  void NewLocalPeer(const string& peer_name, gscoped_ptr<Peer>* peer) {
    QuorumPeerPB peer_pb;
    peer_pb.set_permanent_uuid(peer_name);
    ASSERT_STATUS_OK(Peer::NewLocalPeer(peer_pb,
                                        kTabletId,
                                        peer_name,
                                        message_queue_.get(),
                                        log_.get(),
                                        peer));
  }

  DelayablePeerProxy<NoOpTestPeerProxy>* NewRemotePeer(
      const string& peer_name,
      gscoped_ptr<Peer>* peer) {
    QuorumPeerPB peer_pb;
    peer_pb.set_permanent_uuid(peer_name);
    DelayablePeerProxy<NoOpTestPeerProxy>* proxy_ptr =
        new DelayablePeerProxy<NoOpTestPeerProxy>(pool_.get(),
            new NoOpTestPeerProxy(pool_.get(), peer_pb));
    gscoped_ptr<PeerProxy> proxy(proxy_ptr);
    CHECK_OK(Peer::NewRemotePeer(peer_pb,
                                 kTabletId,
                                 kLeaderUuid,
                                 message_queue_.get(),
                                 proxy.Pass(),
                                 peer));
    return proxy_ptr;
  }

  void CheckLastLogEntry(int term, int index) {
    OpId id;
    log_->GetLastEntryOpId(&id);
    ASSERT_EQ(id.term(), term);
    ASSERT_EQ(id.index(), index);
  }

  void CheckLastRemoteEntry(DelayablePeerProxy<NoOpTestPeerProxy>* proxy, int term, int index) {
    OpId id;
    id.CopyFrom(proxy->proxy()->last_received());
    ASSERT_EQ(id.term(), term);
    ASSERT_EQ(id.index(), index);
  }

  // Registers a callback triggered when the op with the provided term and index
  // is committed in the test consensus impl.
  // This must be called _before_ the operation is committed.
  void WaitForMajorityReplicatedIndex(int index) {
    for (int i = 0; i < 100; i++) {
      if (consensus_->IsMajorityReplicated(index)) {
        return;
      }
      usleep(1000 * i);
    }
    FAIL() << "Never replicated index " << index << " on a majority";
  }

 protected:
  gscoped_ptr<TestRaftConsensusQueueIface> consensus_;
  MetricRegistry metric_registry_;
  MetricContext metric_context_;
  gscoped_ptr<FsManager> fs_manager_;
  gscoped_ptr<Log> log_;
  gscoped_ptr<PeerMessageQueue> message_queue_;
  const Schema schema_;
  LogOptions options_;
  gscoped_ptr<ThreadPool> pool_;
};

// Tests that a local peer is correctly built and tracked
// by the message queue.
// After the operations are considered done the log should
// reflect the replicated messages.
TEST_F(ConsensusPeersTest, TestLocalPeer) {
  message_queue_->Init(consensus_.get(), MinimumOpId(), MinimumOpId().term(), 1);

  gscoped_ptr<Peer> local_peer;

  NewLocalPeer("local-peer", &local_peer);
  // Test that the local peer handles status-only requests.
  local_peer->SignalRequest(true);

  // Append a bunch of messages to the queue
  AppendReplicateMessagesToQueue(message_queue_.get(), 1, 20);

  // The above append ends up appending messages in term 2, so we
  // update the peer's term to match.
  local_peer->SetTermForTest(2);

  // signal the peer there are requests pending.
  local_peer->SignalRequest();
  // Now wait on the last operation, this will complete once the peer has logged all
  // requests.
  WaitForMajorityReplicatedIndex(20);

  // verify that the requests are in fact logged.
  CheckLastLogEntry(2, 20);
}

// Tests that a remote peer is correctly built and tracked
// by the message queue.
// After the operations are considered done the proxy (which
// simulates the other endpoint) should reflect the replicated
// messages.
TEST_F(ConsensusPeersTest, TestRemotePeer) {
  message_queue_->Init(consensus_.get(), MinimumOpId(), MinimumOpId().term(), 1);
  gscoped_ptr<Peer> remote_peer;
  DelayablePeerProxy<NoOpTestPeerProxy>* proxy =
      NewRemotePeer("remote-peer", &remote_peer);

  // Append a bunch of messages to the queue
  AppendReplicateMessagesToQueue(message_queue_.get(), 1, 20);

  // The above append ends up appending messages in term 2, so we
  // update the peer's term to match.
  remote_peer->SetTermForTest(2);

  // signal the peer there are requests pending.
  remote_peer->SignalRequest();
  // now wait on the status of the last operation
  // this will complete once the peer has logged all
  // requests.
  WaitForMajorityReplicatedIndex(20);
  // verify that the replicated watermark corresponds to the last replicated
  // message.
  CheckLastRemoteEntry(proxy, 2, 20);
}

TEST_F(ConsensusPeersTest, TestLocalAndRemotePeers) {
  message_queue_->Init(consensus_.get(), MinimumOpId(), MinimumOpId().term(), 2);
  gscoped_ptr<Peer> local_peer;

  // Create a set of peers
  NewLocalPeer("local-peer", &local_peer);

  gscoped_ptr<Peer> remote_peer1;
  DelayablePeerProxy<NoOpTestPeerProxy>* remote_peer1_proxy =
      NewRemotePeer("remote-peer1", &remote_peer1);

  gscoped_ptr<Peer> remote_peer2;
  DelayablePeerProxy<NoOpTestPeerProxy>* remote_peer2_proxy =
      NewRemotePeer("remote-peer2", &remote_peer2);

  // Delay the response from the second remote peer.
  remote_peer2_proxy->DelayResponse();

  // Append one message to the queue.
  AppendReplicateMessagesToQueue(message_queue_.get(), 1, 1);

  OpId first;
  first.set_term(0);
  first.set_index(1);

  local_peer->SignalRequest();
  remote_peer1->SignalRequest();
  remote_peer2->SignalRequest();

  // Now wait for the message to be replicated, this should succeed since
  // majority = 2 and only one peer was delayed.
  WaitForMajorityReplicatedIndex(first.index());

  CheckLastLogEntry(first.term(), first.index());
  CheckLastRemoteEntry(remote_peer1_proxy, first.term(), first.index());

  ASSERT_STATUS_OK(remote_peer2_proxy->Respond(TestPeerProxy::kUpdate));
  // Wait until all peers have replicated the message, otherwise
  // when we add the next one remote_peer2 might find the next message
  // in the queue and will replicate it, which is not what we want.
  while (!OpIdEquals(message_queue_->GetAllReplicatedIndexForTests(), first)) {
    usleep(1000);
  }

  // Now append another message to the queue
  AppendReplicateMessagesToQueue(message_queue_.get(), 2, 1);

  // Signal a single peer
  remote_peer1->SignalRequest();

  // We should not see it replicated, even after 10ms,
  // since only a single peer replicated the message.
  usleep(10 * 1000);
  ASSERT_FALSE(consensus_->IsMajorityReplicated(2));

  // Signal another peer
  remote_peer2->SignalRequest();
  // We should now be able to wait for it to replicate, since two peers (a majority)
  // have replicated the message.
  WaitForMajorityReplicatedIndex(2);
}

}  // namespace consensus
}  // namespace kudu

