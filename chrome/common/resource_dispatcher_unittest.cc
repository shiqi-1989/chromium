// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include "base/message_loop.h"
#include "base/process.h"
#include "base/process_util.h"
#include "base/scoped_ptr.h"
#include "chrome/common/filter_policy.h"
#include "chrome/common/render_messages.h"
#include "chrome/common/resource_dispatcher.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "webkit/appcache/appcache_interfaces.h"

using webkit_glue::ResourceLoaderBridge;

static const char test_page_url[] = "http://www.google.com/";
static const char test_page_headers[] =
  "HTTP/1.1 200 OK\nContent-Type:text/html\n\n";
static const char test_page_mime_type[] = "text/html";
static const char test_page_charset[] = "";
static const char test_page_contents[] =
  "<html><head><title>Google</title></head><body><h1>Google</h1></body></html>";
static const uint32 test_page_contents_len = arraysize(test_page_contents) - 1;

// Listens for request response data and stores it so that it can be compared
// to the reference data.
class TestRequestCallback : public ResourceLoaderBridge::Peer {
 public:
  TestRequestCallback() : complete_(false) {
  }

  virtual bool OnReceivedRedirect(
      const GURL& new_url,
      const ResourceLoaderBridge::ResponseInfo& info,
      bool* has_new_first_party_for_cookies,
      GURL* new_first_party_for_cookies) {
    *has_new_first_party_for_cookies = false;
    return true;
  }

  virtual void OnReceivedResponse(
      const ResourceLoaderBridge::ResponseInfo& info,
      bool content_filtered) {
  }

  virtual void OnReceivedData(const char* data, int len) {
    EXPECT_FALSE(complete_);
    data_.append(data, len);
  }

  virtual void OnUploadProgress(uint64 position, uint64 size) {
  }

  virtual void OnCompletedRequest(const URLRequestStatus& status,
                                  const std::string& security_info) {
    EXPECT_FALSE(complete_);
    complete_ = true;
  }

  virtual GURL GetURLForDebugging() const {
    return GURL();
  }

  const std::string& data() const {
    return data_;
  }
  bool complete() const {
    return complete_;
  }

 private:
  bool complete_;
  std::string data_;
};


// Sets up the message sender override for the unit test
class ResourceDispatcherTest : public testing::Test,
                               public IPC::Message::Sender {
 public:
  // Emulates IPC send operations (IPC::Message::Sender) by adding
  // pending messages to the queue.
  virtual bool Send(IPC::Message* msg) {
    message_queue_.push_back(IPC::Message(*msg));
    delete msg;
    return true;
  }

  // Emulates the browser process and processes the pending IPC messages,
  // returning the hardcoded file contents.
  void ProcessMessages() {
    while (!message_queue_.empty()) {
      int request_id;
      ViewHostMsg_Resource_Request request;
      ASSERT_TRUE(ViewHostMsg_RequestResource::Read(
          &message_queue_[0], &request_id, &request));

      // check values
      EXPECT_EQ(test_page_url, request.url.spec());

      // received response message
      ResourceResponseHead response;
      std::string raw_headers(test_page_headers);
      std::replace(raw_headers.begin(), raw_headers.end(), '\n', '\0');
      response.headers = new net::HttpResponseHeaders(raw_headers);
      response.mime_type = test_page_mime_type;
      response.charset = test_page_charset;
      response.filter_policy = FilterPolicy::DONT_FILTER;
      dispatcher_->OnReceivedResponse(request_id, response);

      // received data message with the test contents
      base::SharedMemory shared_mem;
      EXPECT_TRUE(shared_mem.Create(std::wstring(),
          false, false, test_page_contents_len));
      EXPECT_TRUE(shared_mem.Map(test_page_contents_len));
      char* put_data_here = static_cast<char*>(shared_mem.memory());
      memcpy(put_data_here, test_page_contents, test_page_contents_len);
      base::SharedMemoryHandle dup_handle;
      EXPECT_TRUE(shared_mem.GiveToProcess(
          base::Process::Current().handle(), &dup_handle));
      dispatcher_->OnReceivedData(
          message_queue_[0], request_id, dup_handle, test_page_contents_len);

      message_queue_.erase(message_queue_.begin());

      // read the ack message.
      Tuple1<int> request_ack;
      ASSERT_TRUE(ViewHostMsg_DataReceived_ACK::Read(
          &message_queue_[0], &request_ack));

      ASSERT_EQ(request_ack.a, request_id);

      message_queue_.erase(message_queue_.begin());
    }
  }

 protected:
  // testing::Test
  virtual void SetUp() {
    dispatcher_.reset(new ResourceDispatcher(this));
  }
  virtual void TearDown() {
    dispatcher_.reset();
  }

  ResourceLoaderBridge* CreateBridge() {
    webkit_glue::ResourceLoaderBridge::RequestInfo request_info;
    request_info.method = "GET";
    request_info.url = GURL(test_page_url);
    request_info.first_party_for_cookies = GURL(test_page_url);
    request_info.referrer = GURL();
    request_info.frame_origin = "null";
    request_info.main_frame_origin = "null";
    request_info.headers = std::string();
    request_info.load_flags = 0;
    request_info.requestor_pid = 0;
    request_info.request_type = ResourceType::SUB_RESOURCE;
    request_info.appcache_host_id = appcache::kNoHostId;
    request_info.routing_id = 0;

    return dispatcher_->CreateBridge(request_info, -1, -1);
  }

  std::vector<IPC::Message> message_queue_;
  static scoped_ptr<ResourceDispatcher> dispatcher_;
};

/*static*/
scoped_ptr<ResourceDispatcher> ResourceDispatcherTest::dispatcher_;

// Does a simple request and tests that the correct data is received.
TEST_F(ResourceDispatcherTest, RoundTrip) {
  TestRequestCallback callback;
  ResourceLoaderBridge* bridge = CreateBridge();

  bridge->Start(&callback);

  ProcessMessages();

  // FIXME(brettw) when the request complete messages are actually handledo
  // and dispatched, uncomment this.
  //EXPECT_TRUE(callback.complete());
  //EXPECT_STREQ(test_page_contents, callback.data().c_str());

  delete bridge;
}

// Tests that the request IDs are straight when there are multiple requests.
TEST_F(ResourceDispatcherTest, MultipleRequests) {
  // FIXME
}

// Tests that the cancel method prevents other messages from being received
TEST_F(ResourceDispatcherTest, Cancel) {
  // FIXME
}

TEST_F(ResourceDispatcherTest, Cookies) {
  // FIXME
}

TEST_F(ResourceDispatcherTest, SerializedPostData) {
  // FIXME
}

// This class provides functionality to validate whether the ResourceDispatcher
// object honors the deferred loading contract correctly, i.e. if deferred
// loading is enabled it should queue up any responses received. If deferred
// loading is enabled/disabled in the context of a dispatched message, other
// queued messages should not be dispatched until deferred load is turned off.
class DeferredResourceLoadingTest : public ResourceDispatcherTest,
                                    public ResourceLoaderBridge::Peer {
 public:
  DeferredResourceLoadingTest()
      : defer_loading_(false) {
  }

  virtual bool Send(IPC::Message* msg) {
    delete msg;
    return true;
  }

  void InitMessages() {
    set_defer_loading(true);

    ResourceResponseHead response_head;
    response_head.status.set_status(URLRequestStatus::SUCCESS);

    IPC::Message* response_message =
        new ViewMsg_Resource_ReceivedResponse(0, 0, response_head);

    dispatcher_->OnMessageReceived(*response_message);

    delete response_message;

    // Duplicate the shared memory handle so both the test and the callee can
    // close their copy.
    base::SharedMemoryHandle duplicated_handle;
    EXPECT_TRUE(shared_handle_.ShareToProcess(base::GetCurrentProcessHandle(),
                                              &duplicated_handle));

    response_message =
        new ViewMsg_Resource_DataReceived(0, 0, duplicated_handle, 100);

    dispatcher_->OnMessageReceived(*response_message);

    delete response_message;

    set_defer_loading(false);
  }

  // ResourceLoaderBridge::Peer methods.
  virtual void OnReceivedResponse(
      const ResourceLoaderBridge::ResponseInfo& info,
      bool content_filtered) {
    EXPECT_EQ(defer_loading_, false);
    set_defer_loading(true);
  }

  virtual bool OnReceivedRedirect(
      const GURL& new_url,
      const ResourceLoaderBridge::ResponseInfo& info,
      bool* has_new_first_party_for_cookies,
      GURL* new_first_party_for_cookies) {
    *has_new_first_party_for_cookies = false;
    return true;
  }

  virtual void OnReceivedData(const char* data, int len) {
    EXPECT_EQ(defer_loading_, false);
    set_defer_loading(false);
  }

  virtual void OnUploadProgress(uint64 position, uint64 size) {
  }

  virtual void OnCompletedRequest(const URLRequestStatus& status,
                                  const std::string& security_info) {
  }

  virtual GURL GetURLForDebugging() const {
    return GURL();
  }

 protected:
  virtual void SetUp() {
    EXPECT_EQ(true, shared_handle_.Create(L"DeferredResourceLoaderTest", false,
                                          false, 100));
    ResourceDispatcherTest::SetUp();
  }

  virtual void TearDown() {
    shared_handle_.Close();
    ResourceDispatcherTest::TearDown();
  }

 private:
  void set_defer_loading(bool defer) {
    defer_loading_ = defer;
    dispatcher_->SetDefersLoading(0, defer);
  }

  bool defer_loading() const {
    return defer_loading_;
  }

  bool defer_loading_;
  base::SharedMemory shared_handle_;
};

TEST_F(DeferredResourceLoadingTest, DeferredLoadTest) {
  MessageLoop message_loop(MessageLoop::TYPE_IO);

  ResourceLoaderBridge* bridge = CreateBridge();

  bridge->Start(this);
  InitMessages();

  // Dispatch deferred messages.
  message_loop.RunAllPending();
  delete bridge;
}

