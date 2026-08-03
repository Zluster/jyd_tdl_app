#include <cassert>
#include <iostream>
#include <vector>

#include "tdl_app/byte_tracker.hpp"

int main() {
  jyd_tracker::ByteTracker::Config config;
  config.high_score = 0.5f;
  config.low_score = 0.1f;
  config.iou_threshold = 0.2f;
  config.max_missed = 2;
  jyd_tracker::ByteTracker tracker(config);

  auto tracks = tracker.update({{10, 10, 50, 90, 0.9f, 0},
                                {150, 10, 190, 90, 0.8f, 0}});
  assert(tracks.size() == 2);
  const auto first_id = tracks[0].id;
  const auto second_id = tracks[1].id;

  tracks = tracker.update({{18, 10, 58, 90, 0.9f, 0},
                           {142, 10, 182, 90, 0.8f, 0}});
  assert(tracks.size() == 2);
  assert(tracks[0].id == first_id && tracks[0].missed == 0);
  assert(tracks[1].id == second_id && tracks[1].missed == 0);

  tracks = tracker.update({{27, 10, 67, 90, 0.2f, 0},
                           {134, 10, 174, 90, 0.8f, 0}});
  assert(tracks[0].id == first_id && tracks[0].missed == 0);
  assert(tracks[1].id == second_id && tracks[1].missed == 0);

  tracks = tracker.update({{126, 10, 166, 90, 0.8f, 0}});
  assert(tracks.size() == 2);
  assert(tracks[0].id == first_id && tracks[0].missed == 1);

  tracks = tracker.update({{45, 10, 85, 90, 0.9f, 0},
                           {118, 10, 158, 90, 0.8f, 0}});
  assert(tracks[0].id == first_id && tracks[0].missed == 0);
  assert(tracks[1].id == second_id && tracks[1].missed == 0);
  std::cout << "byte_tracker_smoke=ok ids=" << first_id << "," << second_id
            << "\n";
  return 0;
}
