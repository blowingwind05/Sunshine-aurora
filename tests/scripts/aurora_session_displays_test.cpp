/**
 * @file tests/scripts/aurora_session_displays_test.cpp
 * @brief Standalone regression for validated commands, capture acknowledgement and lease ownership.
 *
 * Compile with C++23, -DAURORA_SESSION_DISPLAYS_STANDALONE_TEST, -I., and src/aurora_session_displays.cpp.
 */
#ifdef AURORA_SESSION_DISPLAYS_STANDALONE_TEST
#include "src/aurora_session_displays.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
  using namespace aurora::session_display;

  /** @brief Fail the fixture with the applicable ownership check. */
  void require(bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }

  /** @brief Exercise native lifetime, topology readiness and revocation without touching any actual display. */
  class fake_backend final: public backend {
  public:
    mutable unsigned snapshots = 0;
    mutable unsigned creations = 0;
    mutable unsigned closes = 0;
    mutable bool ready = true;
    mutable bool visible = true;
    mutable bool close_succeeds = true;
    mutable bool is_available = true;
    mutable bool mounted = false;
    mutable display_id next_id = 100;
    mutable display_id live_id = 0;
    mutable display_mode requested_mode;
    display_mode primary_mode {3024, 1964, 60};
    double primary_refresh = 59.94;
    mutable bool *revoke_on_create = nullptr;

    /** @brief Simulate one native registration. */
    class monitor final: public lease {
    public:
      monitor(const fake_backend &driver, display_id id): driver {driver}, display {id} {}
      display_id id() const override { return display; }
      bool ready() const override { return driver.mounted && driver.ready; }
      bool close() override {
        ++driver.closes;
        if (!driver.close_succeeds) return false;
        driver.mounted = false;
        return true;
      }
    private:
      const fake_backend &driver;
      display_id display;
    };

    bool available() const override { return is_available; }

    std::optional<std::vector<display>> topology() const override {
      ++snapshots;
      std::vector<display> outputs {
        {10, "Primary", false, true, false, primary_mode.width, primary_mode.height, primary_refresh},
        {20, "Secondary", false, true, false, 1920, 1200, 60},
        {99, "Unrelated lease", true, true, false, 1920, 1080, 60},
      };
      if (mounted && visible) outputs.push_back({live_id, "Owned", true, ready, false,
        requested_mode.width, requested_mode.height, static_cast<double>(requested_mode.fps)});
      return outputs;
    }

    std::unique_ptr<lease> create(display_mode mode, status &) const override {
      ++creations;
      mounted = true;
      live_id = next_id;
      requested_mode = mode;
      if (revoke_on_create) *revoke_on_create = false;
      return std::make_unique<monitor>(*this, live_id);
    }
  };
}

/** @brief Run the offline capture channel and lease regression. */
int main() {
  using enum operation;
  require(parse("operation=create") && parse("operation=create")->mode == display_mode {}, "create follows the active host mode");
  require(parse("operation=create&fps=60&height=1080&width=1920").has_value(), "unordered create");
  require(parse("operation=select&id=4294967295").has_value(), "full Quartz ID");
  for (auto invalid : {"", "operation=list&id=1", "operation=select&id=0", "operation=select&id=4294967296",
                       "operation=select&id=4&id=5", "operation=remove&operation=list&id=4",
                       "operation=create&width=1921&height=1080&fps=60", "operation=create&width=1920&height=1080",
                       "operation=remove&id=%31", "operation=create&id=10", "operation=list&", "operation=select&id=+4", "operation=get"}) {
    require(!parse(invalid), invalid);
  }

  constexpr command create_1080 {create, 0, {1920, 1080, 60}};
  constexpr command to_primary {select, 10};
  constexpr command to_virtual {select, 100};
  constexpr command remove_virtual {remove, 100};
  constexpr command display_list {list};
  fake_backend driver;
  driver.primary_mode = {2560, 1440, 60};
  controller session;
  bool paired = false;
  const auto authorized = [&] { return paired; };
  const auto run = [&](const auto &request) { return session.apply(request, driver, authorized, true); };

  require(run(create_1080).state == status::denied && !driver.snapshots && !driver.creations, "denial before topology");
  paired = true;
  require(run(display_list).state == status::unsupported, "unarmed stream");
  session.capture.enable();
  require(run(display_list).state == status::not_ready && !driver.snapshots, "require real first target");
  require(!session.capture.report_initial(0) && !session.capture.report_initial(10) &&
    session.capture.observe_source(10, false) && session.capture.report_initial(10) &&
    !session.capture.report_initial(20), "one initial capture backed by a real source");
  const auto initial = run(display_list);
  require(initial.active == 10 && initial.can_create && initial.displays.size() == 2, "hide foreign lease");
  require(run(command {select, 99}).state == status::not_found, "foreign virtual target cannot be selected");
  require(run(command {remove, 10}).state == status::not_found, "system displays cannot be removed");

  driver.visible = false;
  auto created = run(create_1080);
  require(created.state == status::not_ready && driver.creations == 1 && session.exclusive(), "native handle without snapshot cannot report success");
  driver.visible = true;
  created = run(create_1080);
  require(created.state == status::ok && created.affected == 100 && created.active == 10 && driver.creations == 1, "idempotent create has actual target");
  const auto incompatible = session.apply(to_virtual, driver, authorized, true, {1920, 1200, 60});
  const auto incompatible_target = std::ranges::find(incompatible.displays, 100, &display::id);
  require(incompatible.state == status::incompatible_aspect && incompatible_target != incompatible.displays.end() &&
    !incompatible_target->selectable && !session.capture.take_pending(), "different negotiated aspect is rejected before capture work");
  driver.primary_mode = {3024, 1964, 60};
  require(run(to_virtual).state == status::incompatible_aspect, "different observed source aspect is rejected");
  driver.primary_mode = {2560, 1440, 60};
  const auto enable_route = [](display_id) { return true; };
  require(!session.capture.presented(1, 100, enable_route), "unsolicited presentation acknowledgement");
  auto selected = run(to_virtual);
  require(selected.state == status::pending && selected.active == 10 && selected.pending == 100, "no success before capture/input confirmation");
  require(!session.capture.expired() && session.capture.expired(std::chrono::steady_clock::now() + std::chrono::seconds {16}),
    "bounded wait for capture confirmation");
  require(run(to_virtual).state == status::pending, "repeat pending select");
  require(run(command {select, 20}).state == status::busy, "competing pending select");
  require(run(remove_virtual).state == status::in_use, "cannot remove pending capture");
  auto ticket = session.capture.take_pending();
  require(ticket.has_value() && !session.capture.take_pending(), "claim only once");
  require(session.capture.begin(*ticket), "capture waits for ordered input pause");
  require(!session.capture.video_allowed(10, 0, true) && !session.capture.video_allowed(100, ticket->generation, true),
    "neither old video nor an unobserved target can cross the boundary");
  std::uint32_t sample_generation = 0;
  require(session.capture.observe_source(100, false, &sample_generation) && sample_generation == ticket->generation,
    "first real exact source is tagged with the requested generation");
  require(run(display_list).generation == ticket->generation && !run(display_list).first_frame &&
    !session.capture.video_allowed(100, ticket->generation, false), "discard dummy and non-IDR output until the first sent target IDR");
  require(!session.capture.presented(ticket->generation, 0xffffffffu, enable_route), "a captured or encoded frame is not yet sent");
  session.capture.video_sent(20, ticket->generation, 20, true);
  session.capture.video_sent(100, ticket->generation, 21, false);
  require(!run(display_list).first_frame, "wrong target and non-IDR cannot be published");
  session.capture.video_sent(100, ticket->generation, 0xffffffffu, true);
  session.capture.video_sent(100, ticket->generation, 1, false);
  require(run(display_list).active == 10 && run(display_list).first_frame == 0xffffffffu, "wire retains old active until barrier");
  require(!session.capture.presented(ticket->generation + 1, 1, enable_route), "different generation cannot be accepted");
  require(!session.capture.presented(ticket->generation, 0, enable_route), "a hole in wrapped frame numbers was never sent");
  require(!session.capture.presented(ticket->generation, 2, enable_route), "future frame was never sent");
  require(!session.capture.presented(ticket->generation, 1, [](display_id) { return false; }), "input route must be ready");
  require(session.capture.presented(ticket->generation, 1, enable_route), "accept an actual wrapped frame in this encrypted generation");
  require(!session.capture.presented(ticket->generation, 1, enable_route), "duplicate presentation cannot thaw input again");
  require(run(to_virtual).state == status::ok && run(display_list).active == 100, "confirm exact and deduplicate completed selection");
  require(run(display_list).can_select, "switch capability remains after confirmation");
  require(run(remove_virtual).state == status::in_use, "cannot remove confirmed live target");
  driver.ready = false;
  require(session.lost_target() && !run(display_list).active, "disappeared target must not be reported active");
  driver.ready = true;
  require(run(to_primary).state == status::pending && run(remove_virtual).state == status::in_use, "protect the previous capture while switching");
  const auto restore = session.capture.take_pending();
  require(restore && restore->generation != ticket->generation && session.capture.begin(*restore) &&
    session.capture.observe_source(10, false), "explicit physical target gets a fresh generation");
  session.capture.video_sent(10, restore->generation, 0, true);
  require(run(display_list).first_frame.has_value() && *run(display_list).first_frame == 0,
    "a real first frame at the uint32 rollover is present and distinct from an absent frame");
  require(!session.capture.presented(ticket->generation, 1, enable_route) &&
    session.capture.presented(restore->generation, 0, enable_route), "old generations cannot enable the new target's route");
  driver.close_succeeds = false;
  require(run(remove_virtual).state == status::cleanup_unconfirmed && driver.closes == 1, "unconfirmed native close cannot report success");
  require(run(to_virtual).state == status::not_ready, "retiring lease is not selectable");
  driver.close_succeeds = true;
  require(run(remove_virtual).state == status::ok && driver.closes == 2, "retry native removal");
  require(run(remove_virtual).state == status::ok && driver.closes == 2, "repeat removal is idempotent");
  require(run(create_1080).state == status::not_ready && driver.closes == 3, "reject Quartz ID reuse within one stream");
  driver.next_id = 101;
  driver.revoke_on_create = &paired;
  require(run(create_1080).state == status::denied && driver.closes == 4 && !driver.mounted, "rollback created lease when authorization has expired");
  paired = true;
  driver.revoke_on_create = nullptr;
  require(run(create_1080).state == status::ok, "fresh authorized create");
  require(run(command {select, 101}).state == status::pending, "shutdown with queued target");
  const auto queued = session.capture.take_pending();
  session.capture.close();
  require(queued && !session.capture.presented(queued->generation, 2, enable_route) && !session.capture.take_pending() && driver.mounted,
    "close requests before joining while keeping the lease alive");
  require(session.cleanup() && !driver.mounted, "release after capture and input teardown");

  controller staging;
  fake_backend staging_driver;
  constexpr command create_default {create};
  const auto stage = [&](const auto &request) { return staging.apply(request, staging_driver, authorized, false); };
  staging.capture.enable();
  encoded_display_source encoder;
  encoder.observe(0, 1, true);
  require(!encoder.for_packet(1) && !staging.capture.report_inventory_ready(10), "placeholder cannot confirm a capture ID");
  require(staging.capture.observe_source(10, true), "first real frame records its Quartz ID");
  encoder.observe(10, 5, true);
  require(!encoder.for_packet(4) && encoder.for_packet(5) == 10 && encoder.for_packet(6) == 10 && encoder.allows_inventory(),
    "delayed placeholders remain untagged while real output and encoded duplicates retain the ID");
  require(stage(display_list).state == status::not_ready && !staging.capture.report_initial(20),
    "sender must confirm the exact observed source before listing");
  require(staging.capture.report_initial(encoder.for_packet(5)), "host source can be confirmed before lease exclusion is published");
  require(stage(display_list).active == 10 && !stage(display_list).can_create, "list works while staging remains gated");
  require(stage(to_virtual).state == status::switch_unavailable && !staging.capture.take_pending(), "production select cannot queue capture work");
  require(stage(create_default).state == status::not_ready && staging_driver.creations == 0, "cannot stage until capture excludes staged leases");
  require(staging.capture.report_inventory_ready(10) && stage(display_list).can_create && !stage(display_list).can_select,
    "inventory and live selection capabilities are independent");
  staging_driver.primary_refresh = 0;
  require(stage(create_default).state == status::mode_unavailable && staging_driver.creations == 0, "unknown refresh does not silently substitute a mode");
  staging_driver.primary_refresh = 59.94;
  staging_driver.primary_mode.width = 3025;
  require(stage(create_default).state == status::mode_unavailable, "an unrepresentable host mode is rejected");
  staging_driver.primary_mode.width = 3024;
  const auto matched = stage(create_default);
  require(matched.state == status::ok && matched.active == 10 && staging_driver.requested_mode == display_mode {3024, 1964, 60},
    "host native non-16:9 dimensions and rounded refresh are retained");
  require(stage(create_default).affected == matched.affected && staging_driver.creations == 1, "default create retries reuse the mode");
  staging.capture.suspend_inventory();
  require(!stage(display_list).active, "drop an unconfirmed source from the published snapshot");
  require(stage(remove_virtual).state == status::not_ready && staging_driver.closes == 0, "do not detach a lease when ordinary capture protection is uncertain");
  require(staging.capture.report_inventory_ready(10) && stage(remove_virtual).state == status::ok, "remove only staged, non-capture lease");
  staging_driver.next_id = 101;
  require(stage(create_1080).state == status::ok && staging_driver.requested_mode == display_mode {1920, 1080, 60}, "explicit override is still available");
  require(!staging.capture.observe_source(20, true) && staging.capture.snapshot().source_lost &&
    !staging.capture.report_inventory_ready(10) && stage(display_list).state == status::not_ready,
    "a changed source revokes inventory and late packets cannot restore it");
  require(staging.cleanup(), "staging lease cleanup");
  std::puts("Aurora display command, capture acknowledgement and lease lifecycle passed");
}
#endif
