// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/tiles/image_controller.h"

#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/optional.h"
#include "base/run_loop.h"
#include "base/test/test_simple_task_runner.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/threading/thread_checker_impl.h"
#include "cc/paint/paint_image_builder.h"
#include "cc/test/skia_common.h"
#include "cc/test/stub_decode_cache.h"
#include "cc/test/test_paint_worklet_input.h"
#include "cc/tiles/image_decode_cache.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cc {
namespace {

class TestWorkerThread : public base::SimpleThread {
 public:
  TestWorkerThread()
      : base::SimpleThread("test_worker_thread"), condition_(&lock_) {}

  void Run() override {
    for (;;) {
      base::OnceClosure task;
      {
        base::AutoLock hold(lock_);
        if (shutdown_)
          break;

        if (queue_.empty()) {
          condition_.Wait();
          continue;
        }

        task = std::move(queue_.front());
        queue_.erase(queue_.begin());
      }
      std::move(task).Run();
    }
  }

  void Shutdown() {
    base::AutoLock hold(lock_);
    shutdown_ = true;
    condition_.Signal();
  }

  void PostTask(base::OnceClosure task) {
    base::AutoLock hold(lock_);
    queue_.push_back(std::move(task));
    condition_.Signal();
  }

 private:
  base::Lock lock_;
  base::ConditionVariable condition_;
  std::vector<base::OnceClosure> queue_;
  bool shutdown_ = false;
};

class WorkerTaskRunner : public base::SequencedTaskRunner {
 public:
  WorkerTaskRunner() { thread_.Start(); }

  bool PostNonNestableDelayedTask(const base::Location& from_here,
                                  base::OnceClosure task,
                                  base::TimeDelta delay) override {
    return PostDelayedTask(from_here, std::move(task), delay);
  }

  bool PostDelayedTask(const base::Location& from_here,
                       base::OnceClosure task,
                       base::TimeDelta delay) override {
    thread_.PostTask(std::move(task));
    return true;
  }

  bool RunsTasksInCurrentSequence() const override { return false; }

 protected:
  ~WorkerTaskRunner() override {
    thread_.Shutdown();
    thread_.Join();
  }

  TestWorkerThread thread_;
};

// Image decode cache with introspection!
class TestableCache : public StubDecodeCache {
 public:
  ~TestableCache() override { EXPECT_EQ(number_of_refs_, 0); }

  TaskResult GetTaskForImageAndRef(const DrawImage& image,
                                   const TracingInfo& tracing_info) override {
    // Return false for large images to mimic "won't fit in memory"
    // behavior.
    if (image.paint_image() &&
        image.paint_image().width() * image.paint_image().height() >=
            1000 * 1000) {
      return TaskResult(false);
    }

    ++number_of_refs_;
    return TaskResult(task_to_use_);
  }
  TaskResult GetOutOfRasterDecodeTaskForImageAndRef(
      const DrawImage& image) override {
    return GetTaskForImageAndRef(image, TracingInfo());
  }

  void UnrefImage(const DrawImage& image) override {
    ASSERT_GT(number_of_refs_, 0);
    --number_of_refs_;
  }
  size_t GetMaximumMemoryLimitBytes() const override {
    return 256 * 1024 * 1024;
  }

  int number_of_refs() const { return number_of_refs_; }
  void SetTaskToUse(scoped_refptr<TileTask> task) { task_to_use_ = task; }

 private:
  int number_of_refs_ = 0;
  scoped_refptr<TileTask> task_to_use_;
};

// A simple class that can receive decode callbacks.
class DecodeClient {
 public:
  DecodeClient() = default;
  void Callback(base::OnceClosure quit_closure,
                ImageController::ImageDecodeRequestId id,
                ImageController::ImageDecodeResult result) {
    id_ = id;
    result_ = result;
    std::move(quit_closure).Run();
  }

  ImageController::ImageDecodeRequestId id() { return id_; }
  ImageController::ImageDecodeResult result() { return result_; }

 private:
  ImageController::ImageDecodeRequestId id_ = 0;
  ImageController::ImageDecodeResult result_ =
      ImageController::ImageDecodeResult::FAILURE;
};

// A dummy task that does nothing.
class SimpleTask : public TileTask {
 public:
  SimpleTask() : TileTask(true /* supports_concurrent_execution */) {
    EXPECT_TRUE(thread_checker_.CalledOnValidThread());
  }
  SimpleTask(const SimpleTask&) = delete;

  SimpleTask& operator=(const SimpleTask&) = delete;

  void RunOnWorkerThread() override {
    EXPECT_FALSE(HasCompleted());
    has_run_ = true;
  }
  void OnTaskCompleted() override {
    EXPECT_TRUE(thread_checker_.CalledOnValidThread());
  }

  bool has_run() { return has_run_; }

 private:
  ~SimpleTask() override = default;

  base::ThreadChecker thread_checker_;
  bool has_run_ = false;
};

// A task that blocks until instructed otherwise.
class BlockingTask : public TileTask {
 public:
  BlockingTask()
      : TileTask(true /* supports_concurrent_execution */), run_cv_(&lock_) {
    EXPECT_TRUE(thread_checker_.CalledOnValidThread());
  }
  BlockingTask(const BlockingTask&) = delete;

  BlockingTask& operator=(const BlockingTask&) = delete;

  void RunOnWorkerThread() override {
    EXPECT_FALSE(HasCompleted());
    EXPECT_FALSE(thread_checker_.CalledOnValidThread());
    base::AutoLock hold(lock_);
    if (!can_run_)
      run_cv_.Wait();
    has_run_ = true;
  }

  void OnTaskCompleted() override {
    EXPECT_TRUE(thread_checker_.CalledOnValidThread());
  }

  void AllowToRun() {
    base::AutoLock hold(lock_);
    can_run_ = true;
    run_cv_.Signal();
  }

  bool has_run() { return has_run_; }

 private:
  ~BlockingTask() override = default;

  // Use ThreadCheckerImpl, so that release builds also get correct behavior.
  base::ThreadCheckerImpl thread_checker_;
  bool has_run_ = false;
  base::Lock lock_;
  base::ConditionVariable run_cv_;
  bool can_run_ = false;
};

// For tests that exercise image controller's thread, this is the timeout value
// to allow the worker thread to do its work.
int kDefaultTimeoutSeconds = 10;

DrawImage CreateDiscardableDrawImage(gfx::Size size) {
  return DrawImage(CreateDiscardablePaintImage(size),
                   SkIRect::MakeWH(size.width(), size.height()),
                   kNone_SkFilterQuality, SkMatrix::I(),
                   PaintImage::kDefaultFrameIndex, gfx::ColorSpace());
}

DrawImage CreateBitmapDrawImage(gfx::Size size) {
  return DrawImage(
      CreateBitmapImage(size), SkIRect::MakeWH(size.width(), size.height()),
      kNone_SkFilterQuality, SkMatrix::I(), PaintImage::kDefaultFrameIndex);
}

class ImageControllerTest : public testing::Test {
 public:
  ImageControllerTest()
      : task_runner_(base::SequencedTaskRunnerHandle::Get()),
        weak_ptr_factory_(this) {
    image_ = CreateDiscardableDrawImage(gfx::Size(1, 1));
  }
  ~ImageControllerTest() override = default;

  void SetUp() override {
    worker_task_runner_ = base::MakeRefCounted<WorkerTaskRunner>();
    controller_.reset(
        new ImageController(task_runner_.get(), worker_task_runner_));
    cache_ = TestableCache();
    controller_->SetImageDecodeCache(&cache_);
  }

  void TearDown() override {
    controller_.reset();
    worker_task_runner_ = nullptr;
    weak_ptr_factory_.InvalidateWeakPtrs();
  }

  base::SequencedTaskRunner* task_runner() { return task_runner_.get(); }
  ImageController* controller() { return controller_.get(); }
  TestableCache* cache() { return &cache_; }
  const DrawImage& image() const { return image_; }

  // Timeout callback, which errors and exits the runloop.
  void Timeout(base::RunLoop* run_loop) {
    ADD_FAILURE() << "Timeout.";
    run_loop->Quit();
  }

  // Convenience method to run the run loop with a timeout.
  void RunOrTimeout(base::RunLoop* run_loop) {
    task_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&ImageControllerTest::Timeout,
                       weak_ptr_factory_.GetWeakPtr(),
                       base::Unretained(run_loop)),
        base::TimeDelta::FromSeconds(kDefaultTimeoutSeconds));
    run_loop->Run();
  }

  void ResetController() { controller_.reset(); }

  SkMatrix CreateMatrix(const SkSize& scale, bool is_decomposable) {
    SkMatrix matrix;
    matrix.setScale(scale.width(), scale.height());

    if (!is_decomposable) {
      // Perspective is not decomposable, add it.
      matrix[SkMatrix::kMPersp0] = 0.1f;
    }

    return matrix;
  }

  PaintImage CreatePaintImage(int width, int height) {
    scoped_refptr<TestPaintWorkletInput> input =
        base::MakeRefCounted<TestPaintWorkletInput>(gfx::SizeF(width, height));
    return CreatePaintWorkletPaintImage(input);
  }

 private:
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  scoped_refptr<WorkerTaskRunner> worker_task_runner_;
  TestableCache cache_;
  std::unique_ptr<ImageController> controller_;
  DrawImage image_;

  base::WeakPtrFactory<ImageControllerTest> weak_ptr_factory_;
};

// Test that GetTasksForImagesAndRef does not generate task for PaintWorklet
// images.
TEST_F(ImageControllerTest, GetTasksForImagesAndRefForPaintWorkletImages) {
  std::vector<DrawImage> images(1);
  ImageDecodeCache::TracingInfo tracing_info;

  PaintImage paint_image = CreatePaintImage(100, 100);
  DrawImage draw_image(
      paint_image, SkIRect::MakeWH(paint_image.width(), paint_image.height()),
      kNone_SkFilterQuality, CreateMatrix(SkSize::Make(1.f, 1.f), true),
      PaintImage::kDefaultFrameIndex);
  images[0] = draw_image;

  ASSERT_EQ(1u, images.size());

  std::vector<scoped_refptr<TileTask>> tasks;
  bool has_at_raster_images = false;
  controller()->ConvertDataImagesToTasks(&images, &tasks, &has_at_raster_images,
                                         tracing_info);
  EXPECT_EQ(tasks.size(), 0u);
}

TEST_F(ImageControllerTest, NullControllerUnrefsImages) {
  std::vector<DrawImage> images(10);
  ImageDecodeCache::TracingInfo tracing_info;

  ASSERT_EQ(10u, images.size());
  auto tasks =
      controller()->SetPredecodeImages(std::move(images), tracing_info);
  EXPECT_EQ(0u, tasks.size());
  EXPECT_EQ(10, cache()->number_of_refs());

  controller()->SetImageDecodeCache(nullptr);
  EXPECT_EQ(0, cache()->number_of_refs());
}

TEST_F(ImageControllerTest, QueueImageDecode) {
  base::RunLoop run_loop;
  DecodeClient decode_client;
  EXPECT_EQ(image().paint_image().width(), 1);
  ImageController::ImageDecodeRequestId expected_id =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client),
                                  run_loop.QuitClosure()));
  RunOrTimeout(&run_loop);
  EXPECT_EQ(expected_id, decode_client.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::SUCCESS,
            decode_client.result());
}

TEST_F(ImageControllerTest, QueueImageDecodeNonLazy) {
  base::RunLoop run_loop;
  DecodeClient decode_client;

  DrawImage image = CreateBitmapDrawImage(gfx::Size(1, 1));

  ImageController::ImageDecodeRequestId expected_id =
      controller()->QueueImageDecode(
          image, base::BindOnce(&DecodeClient::Callback,
                                base::Unretained(&decode_client),
                                run_loop.QuitClosure()));
  RunOrTimeout(&run_loop);
  EXPECT_EQ(expected_id, decode_client.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::DECODE_NOT_REQUIRED,
            decode_client.result());
}

TEST_F(ImageControllerTest, QueueImageDecodeTooLarge) {
  base::RunLoop run_loop;
  DecodeClient decode_client;

  DrawImage image = CreateDiscardableDrawImage(gfx::Size(2000, 2000));
  ImageController::ImageDecodeRequestId expected_id =
      controller()->QueueImageDecode(
          image, base::BindOnce(&DecodeClient::Callback,
                                base::Unretained(&decode_client),
                                run_loop.QuitClosure()));
  RunOrTimeout(&run_loop);
  EXPECT_EQ(expected_id, decode_client.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::FAILURE,
            decode_client.result());
}

TEST_F(ImageControllerTest, QueueImageDecodeMultipleImages) {
  base::RunLoop run_loop;
  DecodeClient decode_client1;
  ImageController::ImageDecodeRequestId expected_id1 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client1),
                                  base::DoNothing::Once()));
  DecodeClient decode_client2;
  ImageController::ImageDecodeRequestId expected_id2 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client2),
                                  base::DoNothing::Once()));
  DecodeClient decode_client3;
  ImageController::ImageDecodeRequestId expected_id3 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client3),
                                  run_loop.QuitClosure()));
  RunOrTimeout(&run_loop);
  EXPECT_EQ(expected_id1, decode_client1.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::SUCCESS,
            decode_client1.result());
  EXPECT_EQ(expected_id2, decode_client2.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::SUCCESS,
            decode_client2.result());
  EXPECT_EQ(expected_id3, decode_client3.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::SUCCESS,
            decode_client3.result());
}

TEST_F(ImageControllerTest, QueueImageDecodeWithTask) {
  scoped_refptr<SimpleTask> task(new SimpleTask);
  cache()->SetTaskToUse(task);

  base::RunLoop run_loop;
  DecodeClient decode_client;
  ImageController::ImageDecodeRequestId expected_id =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client),
                                  run_loop.QuitClosure()));
  RunOrTimeout(&run_loop);
  EXPECT_EQ(expected_id, decode_client.id());
  EXPECT_TRUE(task->has_run());
  EXPECT_TRUE(task->HasCompleted());
}

TEST_F(ImageControllerTest, QueueImageDecodeMultipleImagesSameTask) {
  scoped_refptr<SimpleTask> task(new SimpleTask);
  cache()->SetTaskToUse(task);

  base::RunLoop run_loop;
  DecodeClient decode_client1;
  ImageController::ImageDecodeRequestId expected_id1 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client1),
                                  base::DoNothing::Once()));
  DecodeClient decode_client2;
  ImageController::ImageDecodeRequestId expected_id2 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client2),
                                  base::DoNothing::Once()));
  DecodeClient decode_client3;
  ImageController::ImageDecodeRequestId expected_id3 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client3),
                                  run_loop.QuitClosure()));
  RunOrTimeout(&run_loop);
  EXPECT_EQ(expected_id1, decode_client1.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::SUCCESS,
            decode_client1.result());
  EXPECT_EQ(expected_id2, decode_client2.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::SUCCESS,
            decode_client2.result());
  EXPECT_EQ(expected_id3, decode_client3.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::SUCCESS,
            decode_client3.result());
  EXPECT_TRUE(task->has_run());
  EXPECT_TRUE(task->HasCompleted());
}

TEST_F(ImageControllerTest, QueueImageDecodeChangeControllerWithTaskQueued) {
  scoped_refptr<BlockingTask> task_one(new BlockingTask);
  cache()->SetTaskToUse(task_one);

  DecodeClient decode_client1;
  ImageController::ImageDecodeRequestId expected_id1 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client1),
                                  base::DoNothing::Once()));

  scoped_refptr<BlockingTask> task_two(new BlockingTask);
  cache()->SetTaskToUse(task_two);

  base::RunLoop run_loop;
  DecodeClient decode_client2;
  ImageController::ImageDecodeRequestId expected_id2 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client2),
                                  run_loop.QuitClosure()));

  task_one->AllowToRun();
  task_two->AllowToRun();
  controller()->SetImageDecodeCache(nullptr);
  ResetController();

  RunOrTimeout(&run_loop);

  EXPECT_TRUE(task_one->state().IsCanceled() || task_one->HasCompleted());
  EXPECT_TRUE(task_two->state().IsCanceled() || task_two->HasCompleted());
  EXPECT_EQ(expected_id1, decode_client1.id());
  EXPECT_EQ(expected_id2, decode_client2.id());
}

TEST_F(ImageControllerTest, QueueImageDecodeImageAlreadyLocked) {
  scoped_refptr<SimpleTask> task(new SimpleTask);
  cache()->SetTaskToUse(task);

  base::RunLoop run_loop1;
  DecodeClient decode_client1;
  ImageController::ImageDecodeRequestId expected_id1 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client1),
                                  run_loop1.QuitClosure()));
  RunOrTimeout(&run_loop1);
  EXPECT_EQ(expected_id1, decode_client1.id());
  EXPECT_TRUE(task->has_run());

  cache()->SetTaskToUse(nullptr);
  base::RunLoop run_loop2;
  DecodeClient decode_client2;
  ImageController::ImageDecodeRequestId expected_id2 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client2),
                                  run_loop2.QuitClosure()));
  RunOrTimeout(&run_loop2);
  EXPECT_EQ(expected_id2, decode_client2.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::SUCCESS,
            decode_client2.result());
}

TEST_F(ImageControllerTest, QueueImageDecodeLockedImageControllerChange) {
  scoped_refptr<SimpleTask> task(new SimpleTask);
  cache()->SetTaskToUse(task);

  base::RunLoop run_loop1;
  DecodeClient decode_client1;
  ImageController::ImageDecodeRequestId expected_id1 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client1),
                                  run_loop1.QuitClosure()));
  RunOrTimeout(&run_loop1);
  EXPECT_EQ(expected_id1, decode_client1.id());
  EXPECT_TRUE(task->has_run());
  EXPECT_EQ(1, cache()->number_of_refs());

  controller()->SetImageDecodeCache(nullptr);
  EXPECT_EQ(0, cache()->number_of_refs());
}

TEST_F(ImageControllerTest, DispatchesDecodeCallbacksAfterCacheReset) {
  scoped_refptr<SimpleTask> task(new SimpleTask);
  cache()->SetTaskToUse(task);

  base::RunLoop run_loop1;
  DecodeClient decode_client1;
  base::RunLoop run_loop2;
  DecodeClient decode_client2;

  controller()->QueueImageDecode(
      image(),
      base::BindOnce(&DecodeClient::Callback, base::Unretained(&decode_client1),
                     run_loop1.QuitClosure()));
  controller()->QueueImageDecode(
      image(),
      base::BindOnce(&DecodeClient::Callback, base::Unretained(&decode_client2),
                     run_loop2.QuitClosure()));

  // Now reset the image cache before decode completed callbacks are posted to
  // the compositor thread. Ensure that the completion callbacks for the decode
  // is still run.
  controller()->SetImageDecodeCache(nullptr);
  ResetController();

  RunOrTimeout(&run_loop1);
  RunOrTimeout(&run_loop2);

  EXPECT_EQ(ImageController::ImageDecodeResult::FAILURE,
            decode_client1.result());
  EXPECT_EQ(ImageController::ImageDecodeResult::FAILURE,
            decode_client2.result());
}

TEST_F(ImageControllerTest, DispatchesDecodeCallbacksAfterCacheChanged) {
  scoped_refptr<SimpleTask> task(new SimpleTask);
  cache()->SetTaskToUse(task);

  base::RunLoop run_loop1;
  DecodeClient decode_client1;
  base::RunLoop run_loop2;
  DecodeClient decode_client2;

  controller()->QueueImageDecode(
      image(),
      base::BindOnce(&DecodeClient::Callback, base::Unretained(&decode_client1),
                     run_loop1.QuitClosure()));
  controller()->QueueImageDecode(
      image(),
      base::BindOnce(&DecodeClient::Callback, base::Unretained(&decode_client2),
                     run_loop2.QuitClosure()));

  // Now reset the image cache before decode completed callbacks are posted to
  // the compositor thread. This should orphan the requests.
  controller()->SetImageDecodeCache(nullptr);

  EXPECT_EQ(0, cache()->number_of_refs());

  TestableCache other_cache;
  other_cache.SetTaskToUse(task);

  controller()->SetImageDecodeCache(&other_cache);

  RunOrTimeout(&run_loop1);
  RunOrTimeout(&run_loop2);

  EXPECT_EQ(2, other_cache.number_of_refs());
  EXPECT_EQ(ImageController::ImageDecodeResult::SUCCESS,
            decode_client1.result());
  EXPECT_EQ(ImageController::ImageDecodeResult::SUCCESS,
            decode_client2.result());

  // Reset the controller since the order of destruction is wrong in this test
  // (|other_cache| should outlive the controller. This is normally done via
  // SetImageDecodeCache(nullptr) or it can be done in the dtor of the cache.)
  ResetController();
}

TEST_F(ImageControllerTest, QueueImageDecodeLazyCancelImmediately) {
  DecodeClient decode_client1;
  DecodeClient decode_client2;

  // Create two images so that there is always one that is queued up and
  // not run yet.  This prevents raciness in this test.
  DrawImage image1 = CreateDiscardableDrawImage(gfx::Size(1, 1));
  DrawImage image2 = CreateDiscardableDrawImage(gfx::Size(1, 1));

  ImageController::ImageDecodeRequestId expected_id1 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client1),
                                  base::DoNothing::Once()));

  ImageController::ImageDecodeRequestId expected_id2 =
      controller()->QueueImageDecode(
          image(), base::BindOnce(&DecodeClient::Callback,
                                  base::Unretained(&decode_client2),
                                  base::DoNothing::Once()));

  // This needs a ref because it is lazy.
  EXPECT_EQ(2, cache()->number_of_refs());

  // Instead of running, immediately cancel everything.
  controller()->SetImageDecodeCache(nullptr);

  // This should not crash, and nothing should have run.
  EXPECT_NE(expected_id1, decode_client1.id());
  EXPECT_NE(expected_id2, decode_client2.id());
  EXPECT_EQ(0u, decode_client1.id());
  EXPECT_EQ(0u, decode_client2.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::FAILURE,
            decode_client1.result());
  EXPECT_EQ(ImageController::ImageDecodeResult::FAILURE,
            decode_client2.result());

  // Refs should still be cleaned up.
  EXPECT_EQ(0, cache()->number_of_refs());

  // Explicitly reset the controller so that orphaned task callbacks run
  // while the decode clients still exist.
  ResetController();
}

TEST_F(ImageControllerTest, QueueImageDecodeNonLazyCancelImmediately) {
  DecodeClient decode_client1;
  DecodeClient decode_client2;

  // Create two images so that there is always one that is queued up and
  // not run yet.  This prevents raciness in this test.
  DrawImage image1 = CreateBitmapDrawImage(gfx::Size(1, 1));
  DrawImage image2 = CreateBitmapDrawImage(gfx::Size(1, 1));

  ImageController::ImageDecodeRequestId expected_id1 =
      controller()->QueueImageDecode(
          image1, base::BindOnce(&DecodeClient::Callback,
                                 base::Unretained(&decode_client1),
                                 base::DoNothing::Once()));
  ImageController::ImageDecodeRequestId expected_id2 =
      controller()->QueueImageDecode(
          image2, base::BindOnce(&DecodeClient::Callback,
                                 base::Unretained(&decode_client2),
                                 base::DoNothing::Once()));

  // No ref needed here, because it is non-lazy.
  EXPECT_EQ(0, cache()->number_of_refs());

  // Instead of running, immediately cancel everything.
  controller()->SetImageDecodeCache(nullptr);

  // This should not crash, and nothing should have run.
  EXPECT_NE(expected_id1, decode_client1.id());
  EXPECT_NE(expected_id2, decode_client2.id());
  EXPECT_EQ(0u, decode_client1.id());
  EXPECT_EQ(0u, decode_client2.id());
  EXPECT_EQ(ImageController::ImageDecodeResult::FAILURE,
            decode_client1.result());
  EXPECT_EQ(ImageController::ImageDecodeResult::FAILURE,
            decode_client2.result());
  EXPECT_EQ(0, cache()->number_of_refs());

  // Explicitly reset the controller so that orphaned task callbacks run
  // while the decode clients still exist.
  ResetController();
}

}  // namespace
}  // namespace cc
