#include "tdl_app/byte_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kStateSize = 8;
constexpr int kMeasurementSize = 4;

float &matrix(std::array<float, 64> *values, int row, int column) {
  return (*values)[static_cast<size_t>(row * kStateSize + column)];
}

float matrix(const std::array<float, 64> &values, int row, int column) {
  return values[static_cast<size_t>(row * kStateSize + column)];
}

bool invert4x4(const float input[4][4], float inverse[4][4]) {
  float augmented[4][8]{};
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      augmented[row][column] = input[row][column];
    }
    augmented[row][row + 4] = 1.0f;
  }
  for (int pivot = 0; pivot < 4; ++pivot) {
    int best = pivot;
    for (int row = pivot + 1; row < 4; ++row) {
      if (std::fabs(augmented[row][pivot]) >
          std::fabs(augmented[best][pivot])) {
        best = row;
      }
    }
    if (std::fabs(augmented[best][pivot]) < 1e-8f) return false;
    if (best != pivot) {
      for (int column = 0; column < 8; ++column) {
        std::swap(augmented[pivot][column], augmented[best][column]);
      }
    }
    const float divisor = augmented[pivot][pivot];
    for (int column = 0; column < 8; ++column) {
      augmented[pivot][column] /= divisor;
    }
    for (int row = 0; row < 4; ++row) {
      if (row == pivot) continue;
      const float factor = augmented[row][pivot];
      for (int column = 0; column < 8; ++column) {
        augmented[row][column] -= factor * augmented[pivot][column];
      }
    }
  }
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      inverse[row][column] = augmented[row][column + 4];
    }
  }
  return true;
}

// Returns one globally optimal column for each row. The caller pads the cost
// matrix to square and rejects forbidden or below-threshold matches.
std::vector<int> hungarian(const std::vector<std::vector<float>> &cost) {
  const int size = static_cast<int>(cost.size());
  if (size == 0) return {};
  std::vector<float> row_potential(size + 1, 0.0f);
  std::vector<float> column_potential(size + 1, 0.0f);
  std::vector<int> column_match(size + 1, 0);
  std::vector<int> path(size + 1, 0);
  for (int row = 1; row <= size; ++row) {
    column_match[0] = row;
    int column0 = 0;
    std::vector<float> minimum(size + 1,
                               std::numeric_limits<float>::max());
    std::vector<bool> used(size + 1, false);
    do {
      used[column0] = true;
      const int row0 = column_match[column0];
      float delta = std::numeric_limits<float>::max();
      int column1 = 0;
      for (int column = 1; column <= size; ++column) {
        if (used[column]) continue;
        const float current = cost[static_cast<size_t>(row0 - 1)]
                                  [static_cast<size_t>(column - 1)] -
                              row_potential[row0] -
                              column_potential[column];
        if (current < minimum[column]) {
          minimum[column] = current;
          path[column] = column0;
        }
        if (minimum[column] < delta) {
          delta = minimum[column];
          column1 = column;
        }
      }
      for (int column = 0; column <= size; ++column) {
        if (used[column]) {
          row_potential[column_match[column]] += delta;
          column_potential[column] -= delta;
        } else {
          minimum[column] -= delta;
        }
      }
      column0 = column1;
    } while (column_match[column0] != 0);
    do {
      const int column1 = path[column0];
      column_match[column0] = column_match[column1];
      column0 = column1;
    } while (column0 != 0);
  }
  std::vector<int> assignment(size, -1);
  for (int column = 1; column <= size; ++column) {
    if (column_match[column] > 0) {
      assignment[static_cast<size_t>(column_match[column] - 1)] = column - 1;
    }
  }
  return assignment;
}

}  // namespace

namespace jyd_tracker {

ByteTracker::ByteTracker() : ByteTracker(Config{}) {}

ByteTracker::ByteTracker(Config config) : config_(config) {}

float ByteTracker::iou(const Detection &lhs, const Detection &rhs) {
  const float x1 = std::max(lhs.x1, rhs.x1);
  const float y1 = std::max(lhs.y1, rhs.y1);
  const float x2 = std::min(lhs.x2, rhs.x2);
  const float y2 = std::min(lhs.y2, rhs.y2);
  const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
  const float lhs_area = std::max(0.0f, lhs.x2 - lhs.x1) * std::max(0.0f, lhs.y2 - lhs.y1);
  const float rhs_area = std::max(0.0f, rhs.x2 - rhs.x1) * std::max(0.0f, rhs.y2 - rhs.y1);
  const float denominator = lhs_area + rhs_area - intersection;
  return denominator > 0.0f ? intersection / denominator : 0.0f;
}

void ByteTracker::initializeKalman(Track *track) {
  const float width = std::max(1.0f, track->box.x2 - track->box.x1);
  const float height = std::max(1.0f, track->box.y2 - track->box.y1);
  track->kalman_mean = {{(track->box.x1 + track->box.x2) * 0.5f,
                         (track->box.y1 + track->box.y2) * 0.5f,
                         width, height, 0.0f, 0.0f, 0.0f, 0.0f}};
  track->kalman_covariance.fill(0.0f);
  const float scale = std::max(width, height);
  const float position_std = std::max(1.0f, scale * 0.10f);
  const float velocity_std = std::max(1.0f, scale * 0.0625f);
  for (int i = 0; i < 4; ++i) {
    matrix(&track->kalman_covariance, i, i) = position_std * position_std;
    matrix(&track->kalman_covariance, i + 4, i + 4) =
        velocity_std * velocity_std;
  }
  track->kalman_initialized = true;
}

void ByteTracker::predictKalman(Track *track) {
  if (!track->kalman_initialized) initializeKalman(track);
  track->previous_center_x = (track->box.x1 + track->box.x2) * 0.5f;
  for (int i = 0; i < 4; ++i) {
    track->kalman_mean[static_cast<size_t>(i)] +=
        track->kalman_mean[static_cast<size_t>(i + 4)];
  }

  std::array<float, 64> predicted{};
  for (int row = 0; row < kStateSize; ++row) {
    for (int column = 0; column < kStateSize; ++column) {
      float value = matrix(track->kalman_covariance, row, column);
      if (row < 4) value += matrix(track->kalman_covariance, row + 4, column);
      if (column < 4) {
        value += matrix(track->kalman_covariance, row, column + 4);
        if (row < 4) {
          value += matrix(track->kalman_covariance, row + 4, column + 4);
        }
      }
      matrix(&predicted, row, column) = value;
    }
  }
  const float scale = std::max(1.0f, std::max(track->kalman_mean[2],
                                              track->kalman_mean[3]));
  const float position_noise = std::max(0.5f, scale * 0.05f);
  const float velocity_noise = std::max(0.1f, scale * 0.00625f);
  for (int i = 0; i < 4; ++i) {
    matrix(&predicted, i, i) += position_noise * position_noise;
    matrix(&predicted, i + 4, i + 4) += velocity_noise * velocity_noise;
  }
  track->kalman_covariance = predicted;

  const float width = std::max(1.0f, track->kalman_mean[2]);
  const float height = std::max(1.0f, track->kalman_mean[3]);
  track->box.x1 = track->kalman_mean[0] - width * 0.5f;
  track->box.y1 = track->kalman_mean[1] - height * 0.5f;
  track->box.x2 = track->kalman_mean[0] + width * 0.5f;
  track->box.y2 = track->kalman_mean[1] + height * 0.5f;
  ++track->age;
}

void ByteTracker::updateKalman(Track *track, const Detection &detection) {
  if (!track->kalman_initialized) initializeKalman(track);
  const float measurement[4] = {
      (detection.x1 + detection.x2) * 0.5f,
      (detection.y1 + detection.y2) * 0.5f,
      std::max(1.0f, detection.x2 - detection.x1),
      std::max(1.0f, detection.y2 - detection.y1)};
  const float scale = std::max(measurement[2], measurement[3]);
  const float measurement_noise = std::max(1.0f, scale * 0.05f);
  float innovation_covariance[4][4]{};
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      innovation_covariance[row][column] =
          matrix(track->kalman_covariance, row, column);
    }
    innovation_covariance[row][row] +=
        measurement_noise * measurement_noise;
  }
  float inverse[4][4]{};
  if (!invert4x4(innovation_covariance, inverse)) {
    track->box = detection;
    initializeKalman(track);
    return;
  }
  float gain[8][4]{};
  for (int row = 0; row < 8; ++row) {
    for (int column = 0; column < 4; ++column) {
      for (int value = 0; value < 4; ++value) {
        gain[row][column] += matrix(track->kalman_covariance, row, value) *
                             inverse[value][column];
      }
    }
  }
  float residual[4]{};
  for (int i = 0; i < 4; ++i) {
    residual[i] = measurement[i] - track->kalman_mean[static_cast<size_t>(i)];
  }
  for (int row = 0; row < 8; ++row) {
    for (int column = 0; column < 4; ++column) {
      track->kalman_mean[static_cast<size_t>(row)] +=
          gain[row][column] * residual[column];
    }
  }
  const std::array<float, 64> old_covariance = track->kalman_covariance;
  for (int row = 0; row < 8; ++row) {
    for (int column = 0; column < 8; ++column) {
      float correction = 0.0f;
      for (int value = 0; value < 4; ++value) {
        correction += gain[row][value] *
                      matrix(old_covariance, value, column);
      }
      matrix(&track->kalman_covariance, row, column) =
          matrix(old_covariance, row, column) - correction;
    }
  }
  const float width = std::max(1.0f, track->kalman_mean[2]);
  const float height = std::max(1.0f, track->kalman_mean[3]);
  track->box = detection;
  track->box.x1 = track->kalman_mean[0] - width * 0.5f;
  track->box.y1 = track->kalman_mean[1] - height * 0.5f;
  track->box.x2 = track->kalman_mean[0] + width * 0.5f;
  track->box.y2 = track->kalman_mean[1] + height * 0.5f;
}

void ByteTracker::associate(const std::vector<Detection> &detections,
                            std::vector<bool> *matched_tracks,
                            std::vector<bool> *matched_detections) {
  std::vector<size_t> track_indices;
  for (size_t index = 0; index < tracks_.size(); ++index) {
    if (!(*matched_tracks)[index]) track_indices.push_back(index);
  }
  if (track_indices.empty() || detections.empty()) return;
  const size_t matrix_size = std::max(track_indices.size(), detections.size());
  constexpr float kForbidden = 100000.0f;
  std::vector<std::vector<float>> cost(
      matrix_size, std::vector<float>(matrix_size, 1.0f));
  for (size_t row = 0; row < track_indices.size(); ++row) {
    for (size_t column = 0; column < detections.size(); ++column) {
      const Track &track = tracks_[track_indices[row]];
      if (track.box.class_id != detections[column].class_id) {
        cost[row][column] = kForbidden;
        continue;
      }
      const float overlap = iou(track.box, detections[column]);
      cost[row][column] = overlap >= config_.iou_threshold
                              ? 1.0f - overlap
                              : kForbidden;
    }
  }
  const std::vector<int> assignment = hungarian(cost);
  for (size_t row = 0; row < track_indices.size(); ++row) {
    const int column = assignment[row];
    if (column < 0 || column >= static_cast<int>(detections.size()) ||
        cost[row][static_cast<size_t>(column)] >= kForbidden ||
        (*matched_detections)[static_cast<size_t>(column)]) {
      continue;
    }
    Track &track = tracks_[track_indices[row]];
    updateKalman(&track, detections[static_cast<size_t>(column)]);
    track.missed = 0;
    (*matched_tracks)[track_indices[row]] = true;
    (*matched_detections)[static_cast<size_t>(column)] = true;
  }
}

std::vector<Track> ByteTracker::update(const std::vector<Detection> &detections) {
  std::vector<Detection> high;
  std::vector<Detection> low;
  for (const Detection &detection : detections) {
    if (detection.score >= config_.high_score) high.push_back(detection);
    else if (detection.score >= config_.low_score) low.push_back(detection);
  }
  for (Track &track : tracks_) predictKalman(&track);
  std::vector<bool> matched_tracks(tracks_.size(), false);
  std::vector<bool> matched_high(high.size(), false);
  associate(high, &matched_tracks, &matched_high);
  std::vector<bool> matched_low(low.size(), false);
  associate(low, &matched_tracks, &matched_low);
  for (size_t index = 0; index < tracks_.size(); ++index) {
    if (!matched_tracks[index]) tracks_[index].missed++;
  }
  for (size_t index = 0; index < high.size(); ++index) {
    if (matched_high[index]) continue;
    Track track;
    track.id = next_id_++;
    track.box = high[index];
    track.age = 1;
    track.previous_center_x = (track.box.x1 + track.box.x2) * 0.5f;
    initializeKalman(&track);
    tracks_.push_back(track);
  }
  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                               [&](const Track &track) { return track.missed > config_.max_missed; }),
                tracks_.end());
  return tracks_;
}

int ByteTracker::updateLineCount(float line_x) {
  int added = 0;
  for (Track &track : tracks_) {
    const float center_x = (track.box.x1 + track.box.x2) * 0.5f;
    if (!track.counted && track.previous_center_x < line_x && center_x >= line_x) {
      track.counted = true;
      ++added;
    }
  }
  return added;
}

void ByteTracker::reset() {
  tracks_.clear();
  next_id_ = 1;
}

}  // namespace jyd_tracker
