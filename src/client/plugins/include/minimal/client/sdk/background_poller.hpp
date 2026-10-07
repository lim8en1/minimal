#pragma once

// The mutex/cv/thread plumbing every background-polling client plugin needs
// (cost-view's TotalsFeed, checklist-view's ChecklistFeed): a worker
// thread that runs until told to stop, torn down with a bounded join rather
// than detached (a detached worker would keep calling into ClientApi/
// SessionClient while the poller's own destructor runs -- a shutdown UAF,
// strictly worse than a bounded stall on one in-flight fetch).
//
// EXTRACTED after a real bug: the old ChecklistPoller's worker started from a
// MEMBER INITIALIZER, so it was only safe because the mutex it locks
// (mtx_) happened to be declared earlier in the class -- get the declaration
// order wrong (or add a member the loop touches after the thread member) and
// the worker can lock a mutex, or read a bool, that does not exist yet:
// macOS reported "mutex lock failed: Invalid argument" in ~1 of 5 runs under
// parallel load before this was caught. The old TotalsPoller avoided the
// same class of bug only by accident -- it assigned its thread in the
// constructor BODY, after every member exists, not in the initializer list
// -- with nothing enforcing that either poller (or a future one) kept doing so.
//
// THE FIX: Start() is a normal method, called explicitly from the DERIVED
// class's constructor BODY (never from an initializer list, never from this
// base's own constructor) once every member the run loop touches -- base
// and derived alike -- is guaranteed to exist. mtx_/cv_/stop_ live here in
// the base so they construct before any derived member, structurally, not
// by declaration-order discipline the next edit could quietly break.
//
// TEARDOWN: the derived destructor must call Stop() itself, in its own body,
// BEFORE returning -- not rely on ~BackgroundPoller() to do it. Destruction
// order is derived-first: by the time this base's own destructor would run,
// the derived object's members (the ones the still-running loop reads) are
// already gone. Stop() is idempotent (safe to call more than once, or never,
// if Start() was never called) so a defensive call from ~BackgroundPoller()
// is harmless, but the derived class owns getting the ORDER right.

#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

namespace minimal::client::plugins {

class BackgroundPoller {
protected:
    BackgroundPoller() = default;

    // Defensive only -- see the file comment. The derived destructor should
    // already have called Stop() itself before its own members went away.
    // A worker still running here means the derived class never called Stop()
    // in ITS destructor: by now its members are already destroyed while the
    // worker may still be reading them. Joined anyway -- and said out loud,
    // so the bug shows up in the log rather than as a rare crash.
    ~BackgroundPoller() {
        if (worker_.joinable()) {
            std::fprintf(stderr,
                         "minimal: BackgroundPoller destroyed with its worker running -- the "
                         "derived class must call Stop() in its own destructor\n");
        }
        Stop();
    }

    BackgroundPoller(const BackgroundPoller&) = delete;
    BackgroundPoller& operator=(const BackgroundPoller&) = delete;

    // Call once, from the derived constructor's BODY. `run_loop` is typically
    // `[this] { RunLoop(); }`.
    void Start(std::function<void()> run_loop) { worker_ = std::thread(std::move(run_loop)); }

    // Sets stop_, wakes the loop, joins. Safe to call from the derived
    // destructor even though this base's own destructor also calls it --
    // the second call sees `!worker_.joinable()` and is a no-op.
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    mutable std::mutex mtx_;
    // mutable: derived pollers (TotalsFeed, ChecklistFeed) wake the worker
    // from a const render-path call on a tab switch.
    mutable std::condition_variable cv_;
    bool stop_ = false;

private:
    std::thread worker_;
};

}  // namespace minimal::client::plugins
