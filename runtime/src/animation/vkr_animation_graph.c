#include "animation/vkr_animation_graph.h"
#include <float.h>
#include <math.h>

static bool8_t graph_error(const char **error, const char *message) {
  if (error) {
    *error = message;
  }
  return false_v;
}

static float64_t graph_area(const VkrAnimationGraphSample *a,
                            const VkrAnimationGraphSample *b,
                            const VkrAnimationGraphSample *c) {
  return ((float64_t)b->x - a->x) * ((float64_t)c->y - a->y) -
         ((float64_t)b->y - a->y) * ((float64_t)c->x - a->x);
}

static uint32_t graph_leaves(const VkrAnimationGraph *graph, uint32_t index,
                             uint8_t *marks, uint32_t *leaves) {
  if (marks[index] == 1u) {
    return 0u;
  }
  if (marks[index] == 2u) {
    return leaves[index];
  }
  marks[index] = 1u;
  const VkrAnimationGraphNode *node = &graph->nodes[index];
  uint32_t count = node->kind == VKR_ANIMATION_GRAPH_CLIP           ? 1u
                   : node->kind == VKR_ANIMATION_GRAPH_BLENDSPACE1D ? 2u
                                                                    : 3u;
  if (node->kind == VKR_ANIMATION_GRAPH_BLEND2) {
    uint32_t a = graph_leaves(graph, node->inputs[0], marks, leaves);
    uint32_t b = graph_leaves(graph, node->inputs[1], marks, leaves);
    if (!a || !b || a + b > VKR_ANIMATION_BLEND_SAMPLE_CAPACITY) {
      return 0u;
    }
    count = a + b;
  }
  marks[index] = 2u;
  leaves[index] = count;
  return count;
}

bool8_t vkr_animation_graph_validate(const VkrAnimationGraph *graph,
                                     const VkrAnimationAsset *asset,
                                     const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!graph || !graph->node_count ||
      graph->node_count > VKR_ANIMATION_GRAPH_NODES ||
      graph->root >= graph->node_count ||
      graph->parameter_count > VKR_ANIMATION_GRAPH_PARAMETERS ||
      graph->state_count > VKR_ANIMATION_GRAPH_STATES ||
      graph->transition_count > VKR_ANIMATION_GRAPH_TRANSITIONS ||
      !isfinite(graph->cycle_seconds) || graph->cycle_seconds <= 0.0 ||
      (graph->state_count && graph->initial_state >= graph->state_count) ||
      (!graph->state_count && graph->transition_count)) {
    return graph_error(error,
                       "Animation graph counts, root or cycle are invalid");
  }
  for (uint32_t i = 0; i < graph->parameter_count; ++i) {
    if (!isfinite(graph->parameters[i])) {
      return graph_error(error, "Non-finite graph parameter");
    }
  }
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    const VkrAnimationGraphNode *node = &graph->nodes[i];
    if (node->sample_count > VKR_ANIMATION_GRAPH_SAMPLES ||
        node->triangle_count > VKR_ANIMATION_GRAPH_TRIANGLES) {
      return graph_error(error,
                         "Graph node exceeds sample or triangle capacity");
    }
    if (node->kind < VKR_ANIMATION_GRAPH_CLIP ||
        node->kind > VKR_ANIMATION_GRAPH_BLENDSPACE2D) {
      return graph_error(error, "Unknown animation graph node kind");
    }
    if (node->kind == VKR_ANIMATION_GRAPH_CLIP) {
      if (asset && node->clip >= asset->clip_count) {
        return graph_error(error, "Graph clip is missing from bank");
      }
      continue;
    }
    if (node->parameter_x >= graph->parameter_count ||
        (node->kind == VKR_ANIMATION_GRAPH_BLENDSPACE2D &&
         node->parameter_y >= graph->parameter_count)) {
      return graph_error(error, "Graph parameter reference is invalid");
    }
    if (node->kind == VKR_ANIMATION_GRAPH_BLEND2) {
      if (node->inputs[0] >= graph->node_count ||
          node->inputs[1] >= graph->node_count) {
        return graph_error(error, "Graph input reference is invalid");
      }
      continue;
    }
    if (!node->sample_count ||
        node->sample_count > VKR_ANIMATION_GRAPH_SAMPLES ||
        node->triangle_count > VKR_ANIMATION_GRAPH_TRIANGLES) {
      return graph_error(error, "Blend space sample count is invalid");
    }
    for (uint32_t j = 0; j < node->sample_count; ++j) {
      const VkrAnimationGraphSample *sample = &node->samples[j];
      if (!isfinite(sample->x) || !isfinite(sample->y) ||
          (asset && sample->clip >= asset->clip_count)) {
        return graph_error(error, "Blend space sample is invalid");
      }
      for (uint32_t k = 0; k < j; ++k) {
        if (sample->x == node->samples[k].x &&
            (node->kind == VKR_ANIMATION_GRAPH_BLENDSPACE1D ||
             sample->y == node->samples[k].y)) {
          return graph_error(error,
                             "Blend space sample coordinates must be unique");
        }
      }
    }
    if (node->kind == VKR_ANIMATION_GRAPH_BLENDSPACE2D) {
      if (node->sample_count < 3u || !node->triangle_count) {
        return graph_error(error, "2D blend space needs triangles");
      }
      uint32_t used = 0u;
      for (uint32_t j = 0; j < node->triangle_count; ++j) {
        const uint32_t *t = node->triangles[j].samples;
        if (t[0] >= node->sample_count || t[1] >= node->sample_count ||
            t[2] >= node->sample_count || t[0] == t[1] || t[0] == t[2] ||
            t[1] == t[2] ||
            fabs(graph_area(&node->samples[t[0]], &node->samples[t[1]],
                            &node->samples[t[2]])) <= 1e-12) {
          return graph_error(
              error,
              "2D blend triangle is degenerate or references a missing sample");
        }
        used |= (1u << t[0]) | (1u << t[1]) | (1u << t[2]);
      }
      if (used != (1u << node->sample_count) - 1u) {
        return graph_error(error, "Every 2D sample must belong to a triangle");
      }
    }
  }
  uint8_t marks[VKR_ANIMATION_GRAPH_NODES] = {0};
  uint32_t leaves[VKR_ANIMATION_GRAPH_NODES] = {0};
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    if (!graph_leaves(graph, i, marks, leaves)) {
      return graph_error(
          error, "Graph has a pose cycle or exceeds contribution capacity");
    }
  }
  for (uint32_t i = 0; i < graph->state_count; ++i) {
    if (graph->states[i].root >= graph->node_count ||
        !isfinite(graph->states[i].cycle_seconds) ||
        graph->states[i].cycle_seconds <= 0.0) {
      return graph_error(error, "Graph state is invalid");
    }
  }
  for (uint32_t i = 0; i < graph->transition_count; ++i) {
    const VkrAnimationGraphTransition *t = &graph->transitions[i];
    if (t->from >= graph->state_count || t->to >= graph->state_count ||
        t->from == t->to || t->parameter >= graph->parameter_count ||
        (t->comparison != VKR_ANIMATION_GRAPH_GREATER_EQUAL &&
         t->comparison != VKR_ANIMATION_GRAPH_LESS_EQUAL) ||
        !isfinite(t->threshold) || !isfinite(t->duration) ||
        t->duration < 0.0 || !isfinite(t->exit_time) || t->exit_time > 1.0 ||
        leaves[graph->states[t->from].root] +
                leaves[graph->states[t->to].root] >
            VKR_ANIMATION_BLEND_SAMPLE_CAPACITY) {
      return graph_error(
          error,
          "Graph transition is invalid or exceeds contribution capacity");
    }
  }
  return true_v;
}

bool8_t vkr_animation_graph_initialize(VkrAnimationGraphInstance *instance,
                                       const VkrAnimationGraph *graph,
                                       const VkrAnimationAsset *asset,
                                       const char **error) {
  if (!instance || !asset ||
      !vkr_animation_graph_validate(graph, asset, error)) {
    return false_v;
  }
  instance->graph = *graph;
  instance->asset = asset;
  MemCopy(instance->parameters, graph->parameters,
          sizeof(instance->parameters));
  instance->time = instance->state_time = instance->target_time =
      instance->transition_time = 0.0;
  instance->time_compensation = 0.0;
  instance->state_compensation = 0.0;
  instance->target_compensation = 0.0;
  instance->transition_compensation = 0.0;
  instance->state = graph->initial_state;
  instance->transition = 0u;
  instance->transitioning = false_v;
  instance->discontinuity = true_v;
  return true_v;
}

bool8_t vkr_animation_graph_set_parameter(VkrAnimationGraphInstance *instance,
                                          uint32_t parameter, float32_t value) {
  if (!instance || !instance->asset ||
      parameter >= instance->graph.parameter_count || !isfinite(value)) {
    return false_v;
  }
  instance->parameters[parameter] = value;
  return true_v;
}

static bool8_t graph_sample(VkrAnimationSample *samples, uint32_t *count,
                            const VkrAnimationAsset *asset, uint32_t clip,
                            float64_t phase, float64_t weight) {
  if (weight <= 0.0) {
    return true_v;
  }
  float64_t time = phase * asset->clips[clip].duration;
  for (uint32_t i = 0u; i < *count; ++i) {
    if (samples[i].clip == clip && samples[i].time == time) {
      samples[i].weight += (float32_t)weight;
      return true_v;
    }
  }
  if (*count >= VKR_ANIMATION_BLEND_SAMPLE_CAPACITY) {
    return false_v;
  }
  samples[(*count)++] = (VkrAnimationSample){
      .clip = clip, .time = time, .weight = (float32_t)weight};
  return true_v;
}

static bool8_t graph_node(const VkrAnimationGraphInstance *instance,
                          uint32_t index, float64_t phase, float64_t weight,
                          VkrAnimationSample *samples, uint32_t *count) {
  if (weight <= 0.0) {
    return true_v;
  }
  const VkrAnimationGraphNode *node = &instance->graph.nodes[index];
  if (node->kind == VKR_ANIMATION_GRAPH_CLIP) {
    return graph_sample(samples, count, instance->asset, node->clip, phase,
                        weight);
  }
  float64_t x = instance->parameters[node->parameter_x];
  if (node->kind == VKR_ANIMATION_GRAPH_BLEND2) {
    float64_t blend = fmin(1.0, fmax(0.0, x));
    return graph_node(instance, node->inputs[0], phase, weight * (1.0 - blend),
                      samples, count) &&
           graph_node(instance, node->inputs[1], phase, weight * blend, samples,
                      count);
  }
  if (node->kind == VKR_ANIMATION_GRAPH_BLENDSPACE1D) {
    uint32_t low = 0u, high = 0u;
    float64_t low_x = -DBL_MAX, high_x = DBL_MAX;
    for (uint32_t i = 0; i < node->sample_count; ++i) {
      float64_t coordinate = node->samples[i].x;
      if (coordinate <= x && coordinate > low_x) {
        low = i;
        low_x = coordinate;
      }
      if (coordinate >= x && coordinate < high_x) {
        high = i;
        high_x = coordinate;
      }
    }
    if (low_x == -DBL_MAX) {
      low = high;
    }
    if (high_x == DBL_MAX) {
      high = low;
    }
    float64_t blend =
        low == high
            ? 0.0
            : (x - node->samples[low].x) /
                  ((float64_t)node->samples[high].x - node->samples[low].x);
    return graph_sample(samples, count, instance->asset,
                        node->samples[low].clip, phase,
                        weight * (1.0 - blend)) &&
           graph_sample(samples, count, instance->asset,
                        node->samples[high].clip, phase, weight * blend);
  }
  float64_t y = instance->parameters[node->parameter_y];
  uint32_t selected[3] = {0};
  float64_t barycentric[3] = {0};
  float64_t nearest = DBL_MAX;
  for (uint32_t i = 0; i < node->triangle_count; ++i) {
    const uint32_t *t = node->triangles[i].samples;
    const VkrAnimationGraphSample *a = &node->samples[t[0]],
                                  *b = &node->samples[t[1]],
                                  *c = &node->samples[t[2]];
    float64_t area = graph_area(a, b, c);
    float64_t u = (((float64_t)b->x - x) * ((float64_t)c->y - y) -
                   ((float64_t)b->y - y) * ((float64_t)c->x - x)) /
                  area;
    float64_t v = (((float64_t)c->x - x) * ((float64_t)a->y - y) -
                   ((float64_t)c->y - y) * ((float64_t)a->x - x)) /
                  area;
    float64_t w = 1.0 - u - v;
    if (u >= 0.0 && v >= 0.0 && w >= 0.0) {
      MemCopy(selected, t, sizeof(selected));
      barycentric[0] = u;
      barycentric[1] = v;
      barycentric[2] = w;
      break;
    }
    for (uint32_t edge = 0u; edge < 3u; ++edge) {
      uint32_t end = (edge + 1u) % 3u;
      const VkrAnimationGraphSample *p = &node->samples[t[edge]],
                                    *q = &node->samples[t[end]];
      float64_t dx = (float64_t)q->x - p->x, dy = (float64_t)q->y - p->y;
      float64_t alpha =
          fmin(1.0, fmax(0.0, ((x - p->x) * dx + (y - p->y) * dy) /
                                  (dx * dx + dy * dy)));
      float64_t ex = x - (p->x + alpha * dx), ey = y - (p->y + alpha * dy);
      float64_t distance = ex * ex + ey * ey;
      if (distance < nearest) {
        nearest = distance;
        MemCopy(selected, t, sizeof(selected));
        MemZero(barycentric, sizeof(barycentric));
        barycentric[edge] = 1.0 - alpha;
        barycentric[end] = alpha;
      }
    }
  }
  for (uint32_t i = 0; i < 3u; ++i) {
    if (!graph_sample(samples, count, instance->asset,
                      node->samples[selected[i]].clip, phase,
                      weight * barycentric[i])) {
      return false_v;
    }
  }
  return true_v;
}

typedef struct GraphClock {
  float64_t time;
  float64_t state_time;
  float64_t target_time;
  float64_t transition_time;
  float64_t time_compensation;
  float64_t state_compensation;
  float64_t target_compensation;
  float64_t transition_compensation;
  uint32_t state;
  uint32_t transition;
  bool8_t transitioning;
} GraphClock;

static GraphClock graph_clock(const VkrAnimationGraphInstance *instance) {
  return (GraphClock){.time = instance->time,
                      .state_time = instance->state_time,
                      .target_time = instance->target_time,
                      .transition_time = instance->transition_time,
                      .time_compensation = instance->time_compensation,
                      .state_compensation = instance->state_compensation,
                      .target_compensation = instance->target_compensation,
                      .transition_compensation =
                          instance->transition_compensation,
                      .state = instance->state,
                      .transition = instance->transition,
                      .transitioning = instance->transitioning};
}

static bool8_t graph_publish(VkrAnimationGraphInstance *instance,
                             VkrAnimationPlayer *player, GraphClock clock,
                             bool8_t discontinuity) {
  if (!isfinite(clock.time) || !isfinite(clock.state_time) ||
      !isfinite(clock.target_time) || !isfinite(clock.transition_time) ||
      !instance || !instance->asset || !player ||
      vkr_animation_player_asset(player) != instance->asset) {
    return false_v;
  }
  const VkrAnimationGraph *graph = &instance->graph;
  VkrAnimationSample samples[VKR_ANIMATION_BLEND_SAMPLE_CAPACITY];
  uint32_t count = 0u;
  uint32_t root =
      graph->state_count ? graph->states[clock.state].root : graph->root;
  float64_t cycle = graph->state_count
                        ? graph->states[clock.state].cycle_seconds
                        : graph->cycle_seconds;
  float64_t blend = clock.transitioning
                        ? clock.transition_time /
                              graph->transitions[clock.transition].duration
                        : 0.0;
  if (!graph_node(instance, root, fmod(clock.state_time, cycle) / cycle,
                  1.0 - blend, samples, &count)) {
    return false_v;
  }
  if (clock.transitioning) {
    uint32_t target = graph->transitions[clock.transition].to;
    cycle = graph->states[target].cycle_seconds;
    if (!graph_node(instance, graph->states[target].root,
                    fmod(clock.target_time, cycle) / cycle, blend, samples,
                    &count)) {
      return false_v;
    }
  }
  if (!vkr_animation_player_sample_blend(player, samples, count,
                                         discontinuity)) {
    return false_v;
  }
  instance->time = clock.time;
  instance->state_time = clock.state_time;
  instance->target_time = clock.target_time;
  instance->transition_time = clock.transition_time;
  instance->time_compensation = clock.time_compensation;
  instance->state_compensation = clock.state_compensation;
  instance->target_compensation = clock.target_compensation;
  instance->transition_compensation = clock.transition_compensation;
  instance->state = clock.state;
  instance->transition = clock.transition;
  instance->transitioning = clock.transitioning;
  instance->discontinuity = false_v;
  return true_v;
}

static void graph_add_time(float64_t *time, float64_t *compensation,
                           float64_t dt) {
  float64_t corrected = dt - *compensation;
  float64_t next = *time + corrected;
  *compensation = (next - *time) - corrected;
  *time = next;
}

static void graph_tick(float64_t *time, float64_t *compensation, float64_t dt,
                       float64_t cycle, bool8_t *discontinuity) {
  float64_t old_phase = fmod(*time, cycle);
  graph_add_time(time, compensation, dt);
  if (dt > 0.0 && (dt >= cycle || fmod(*time, cycle) < old_phase)) {
    *discontinuity = true_v;
  }
}

static bool8_t graph_advance_clock(const VkrAnimationGraphInstance *instance,
                                   GraphClock *clock, float64_t dt,
                                   bool8_t *discontinuity) {
  const VkrAnimationGraph *graph = &instance->graph;
  if (!isfinite(dt) || dt < 0.0 || !isfinite(clock->time + dt) ||
      !isfinite(clock->state_time + dt) || !isfinite(clock->target_time + dt)) {
    return false_v;
  }
  graph_add_time(&clock->time, &clock->time_compensation, dt);
  if (!graph->state_count) {
    graph_tick(&clock->state_time, &clock->state_compensation, dt,
               graph->cycle_seconds, discontinuity);
    return true_v;
  }
  float64_t remaining = dt;
  for (uint32_t step = 0u; step < 128u; ++step) {
    if (clock->transitioning) {
      const VkrAnimationGraphTransition *t =
          &graph->transitions[clock->transition];
      float64_t consumed =
          fmin(remaining, t->duration - clock->transition_time);
      graph_tick(&clock->state_time, &clock->state_compensation, consumed,
                 graph->states[clock->state].cycle_seconds, discontinuity);
      graph_tick(&clock->target_time, &clock->target_compensation, consumed,
                 graph->states[t->to].cycle_seconds, discontinuity);
      graph_add_time(&clock->transition_time, &clock->transition_compensation,
                     consumed);
      remaining -= consumed;
      if (clock->transition_time < t->duration) {
        return true_v;
      }
      clock->state = t->to;
      clock->state_time = clock->target_time;
      clock->state_compensation = clock->target_compensation;
      clock->transitioning = false_v;
    }
    uint32_t selected = UINT32_MAX;
    float64_t earliest = remaining;
    for (uint32_t i = 0; i < graph->transition_count; ++i) {
      const VkrAnimationGraphTransition *t = &graph->transitions[i];
      float32_t parameter = instance->parameters[t->parameter];
      if (t->from != clock->state ||
          (t->comparison == VKR_ANIMATION_GRAPH_GREATER_EQUAL
               ? parameter < t->threshold
               : parameter > t->threshold)) {
        continue;
      }
      float64_t delay =
          t->exit_time < 0.0
              ? 0.0
              : fmax(0.0,
                     t->exit_time * graph->states[clock->state].cycle_seconds -
                         clock->state_time);
      if (delay <= remaining && (selected == UINT32_MAX || delay < earliest)) {
        selected = i;
        earliest = delay;
      }
    }
    if (selected == UINT32_MAX) {
      graph_tick(&clock->state_time, &clock->state_compensation, remaining,
                 graph->states[clock->state].cycle_seconds, discontinuity);
      return true_v;
    }
    graph_tick(&clock->state_time, &clock->state_compensation, earliest,
               graph->states[clock->state].cycle_seconds, discontinuity);
    remaining -= earliest;
    clock->transition = selected;
    clock->target_time = 0.0;
    clock->transition_time = 0.0;
    clock->target_compensation = 0.0;
    clock->transition_compensation = 0.0;
    const VkrAnimationGraphTransition *t = &graph->transitions[selected];
    if (t->duration > 0.0) {
      clock->transitioning = true_v;
      if (remaining == 0.0) {
        return true_v;
      }
    } else {
      clock->state = t->to;
      clock->state_time = 0.0;
      clock->state_compensation = 0.0;
      *discontinuity = true_v;
    }
  }
  /* Cyclic zero-duration control or excessive catch-up fails transactionally.
   */
  return false_v;
}

bool8_t vkr_animation_graph_evaluate(VkrAnimationGraphInstance *instance,
                                     VkrAnimationPlayer *player) {
  return instance && graph_publish(instance, player, graph_clock(instance),
                                   instance->discontinuity);
}

bool8_t vkr_animation_graph_advance(VkrAnimationGraphInstance *instance,
                                    VkrAnimationPlayer *player, float64_t dt) {
  if (!instance || !instance->asset) {
    return false_v;
  }
  GraphClock clock = graph_clock(instance);
  bool8_t discontinuity = instance->discontinuity;
  return graph_advance_clock(instance, &clock, dt, &discontinuity) &&
         graph_publish(instance, player, clock, discontinuity);
}

bool8_t vkr_animation_graph_seek(VkrAnimationGraphInstance *instance,
                                 VkrAnimationPlayer *player,
                                 float64_t seconds) {
  if (!instance || !instance->asset) {
    return false_v;
  }
  GraphClock clock = {.state = instance->graph.initial_state};
  bool8_t discontinuity = true_v;
  return graph_advance_clock(instance, &clock, seconds, &discontinuity) &&
         graph_publish(instance, player, clock, true_v);
}

bool8_t vkr_animation_graph_reset(VkrAnimationGraphInstance *instance,
                                  VkrAnimationPlayer *player) {
  return vkr_animation_graph_seek(instance, player, 0.0);
}

static bool8_t graph_json_char(VkrJsonReader *reader, char value) {
  vkr_json_skip_whitespace(reader);
  if (reader->pos >= reader->length ||
      reader->data[reader->pos] != (uint8_t)value) {
    return false_v;
  }
  reader->pos++;
  return true_v;
}

static bool8_t graph_json_number(VkrJsonReader *reader, float64_t *value) {
  return vkr_json_parse_double(reader, value) && isfinite(*value);
}

static bool8_t graph_json_uint(VkrJsonReader *reader, uint32_t *value) {
  float64_t number;
  if (!graph_json_number(reader, &number) || number < 0.0 ||
      number > UINT32_MAX || floor(number) != number) {
    return false_v;
  }
  *value = (uint32_t)number;
  return true_v;
}

static bool8_t graph_json_float(VkrJsonReader *reader, float32_t *value) {
  float64_t number;
  if (!graph_json_number(reader, &number) || fabs(number) > FLT_MAX) {
    return false_v;
  }
  *value = (float32_t)number;
  return true_v;
}

static bool8_t graph_json_node(VkrJsonReader *reader,
                               VkrAnimationGraphNode *node) {
  uint32_t kind;
  if (!graph_json_char(reader, '[') || !graph_json_uint(reader, &kind) ||
      !graph_json_char(reader, ',') || !graph_json_uint(reader, &node->clip) ||
      !graph_json_char(reader, ',') ||
      !graph_json_uint(reader, &node->inputs[0]) ||
      !graph_json_char(reader, ',') ||
      !graph_json_uint(reader, &node->inputs[1]) ||
      !graph_json_char(reader, ',') ||
      !graph_json_uint(reader, &node->parameter_x) ||
      !graph_json_char(reader, ',') ||
      !graph_json_uint(reader, &node->parameter_y) ||
      !graph_json_char(reader, ',') || !graph_json_char(reader, '[')) {
    return false_v;
  }
  node->kind = (VkrAnimationGraphNodeKind)kind;
  while (!graph_json_char(reader, ']')) {
    if (node->sample_count == VKR_ANIMATION_GRAPH_SAMPLES ||
        (node->sample_count && !graph_json_char(reader, ','))) {
      return false_v;
    }
    VkrAnimationGraphSample *sample = &node->samples[node->sample_count++];
    if (!graph_json_char(reader, '[') ||
        !graph_json_uint(reader, &sample->clip) ||
        !graph_json_char(reader, ',') ||
        !graph_json_float(reader, &sample->x) ||
        !graph_json_char(reader, ',') ||
        !graph_json_float(reader, &sample->y) ||
        !graph_json_char(reader, ']')) {
      return false_v;
    }
  }
  if (!graph_json_char(reader, ',') || !graph_json_char(reader, '[')) {
    return false_v;
  }
  while (!graph_json_char(reader, ']')) {
    if (node->triangle_count == VKR_ANIMATION_GRAPH_TRIANGLES ||
        (node->triangle_count && !graph_json_char(reader, ','))) {
      return false_v;
    }
    uint32_t *t = node->triangles[node->triangle_count++].samples;
    if (!graph_json_char(reader, '[') || !graph_json_uint(reader, &t[0]) ||
        !graph_json_char(reader, ',') || !graph_json_uint(reader, &t[1]) ||
        !graph_json_char(reader, ',') || !graph_json_uint(reader, &t[2]) ||
        !graph_json_char(reader, ']')) {
      return false_v;
    }
  }
  return graph_json_char(reader, ']');
}

bool8_t vkr_animation_graph_read_json(VkrJsonReader *reader,
                                      const VkrAnimationAsset *asset,
                                      VkrAnimationGraph *out,
                                      const char **error) {
  if (!reader || !out) {
    return graph_error(error, "Missing graph JSON input");
  }
  VkrJsonReader scan = *reader;
  VkrAnimationGraph graph = {0};
  uint32_t fields = 0u;
  if (!graph_json_char(&scan, '{')) {
    return graph_error(error, "Graph JSON must be an object");
  }
  while (!graph_json_char(&scan, '}')) {
    if (fields && !graph_json_char(&scan, ',')) {
      goto malformed;
    }
    String8 key;
    if (!vkr_json_parse_string(&scan, &key) || !graph_json_char(&scan, ':')) {
      goto malformed;
    }
    static const char *names[] = {"version", "cycle",      "root",
                                  "initial", "parameters", "nodes",
                                  "states",  "transitions"};
    uint32_t field = 0u;
    for (; field < ArrayCount(names); ++field) {
      if (vkr_string8_equals_cstr(&key, names[field])) {
        break;
      }
    }
    if (field == ArrayCount(names) || (fields & (1u << field))) {
      goto malformed;
    }
    fields |= 1u << field;
    if (field == 0u) {
      uint32_t version;
      if (!graph_json_uint(&scan, &version) || version != 1u) {
        goto malformed;
      }
    } else if (field == 1u) {
      if (!graph_json_number(&scan, &graph.cycle_seconds)) {
        goto malformed;
      }
    } else if (field == 2u) {
      if (!graph_json_uint(&scan, &graph.root)) {
        goto malformed;
      }
    } else if (field == 3u) {
      if (!graph_json_uint(&scan, &graph.initial_state)) {
        goto malformed;
      }
    } else {
      if (!graph_json_char(&scan, '[')) {
        goto malformed;
      }
      uint32_t count = 0u;
      while (!graph_json_char(&scan, ']')) {
        if (count && !graph_json_char(&scan, ',')) {
          goto malformed;
        }
        if (field == 4u) {
          if (count >= VKR_ANIMATION_GRAPH_PARAMETERS ||
              !graph_json_float(&scan, &graph.parameters[count])) {
            goto malformed;
          }
        } else if (field == 5u) {
          if (count >= VKR_ANIMATION_GRAPH_NODES ||
              !graph_json_node(&scan, &graph.nodes[count])) {
            goto malformed;
          }
        } else if (field == 6u) {
          if (count >= VKR_ANIMATION_GRAPH_STATES ||
              !graph_json_char(&scan, '[') ||
              !graph_json_uint(&scan, &graph.states[count].root) ||
              !graph_json_char(&scan, ',') ||
              !graph_json_number(&scan, &graph.states[count].cycle_seconds) ||
              !graph_json_char(&scan, ']')) {
            goto malformed;
          }
        } else {
          if (count >= VKR_ANIMATION_GRAPH_TRANSITIONS) {
            goto malformed;
          }
          VkrAnimationGraphTransition *t = &graph.transitions[count];
          uint32_t comparison;
          if (!graph_json_char(&scan, '[') ||
              !graph_json_uint(&scan, &t->from) ||
              !graph_json_char(&scan, ',') || !graph_json_uint(&scan, &t->to) ||
              !graph_json_char(&scan, ',') ||
              !graph_json_uint(&scan, &t->parameter) ||
              !graph_json_char(&scan, ',') ||
              !graph_json_uint(&scan, &comparison) ||
              !graph_json_char(&scan, ',') ||
              !graph_json_float(&scan, &t->threshold) ||
              !graph_json_char(&scan, ',') ||
              !graph_json_number(&scan, &t->duration) ||
              !graph_json_char(&scan, ',') ||
              !graph_json_number(&scan, &t->exit_time) ||
              !graph_json_char(&scan, ']')) {
            goto malformed;
          }
          t->comparison = (VkrAnimationGraphComparison)comparison;
        }
        count++;
      }
      if (field == 4u) {
        graph.parameter_count = count;
      } else if (field == 5u) {
        graph.node_count = count;
      } else if (field == 6u) {
        graph.state_count = count;
      } else {
        graph.transition_count = count;
      }
    }
  }
  if (fields != 255u) {
    goto malformed;
  }
  if (!vkr_animation_graph_validate(&graph, asset, error)) {
    return false_v;
  }
  *out = graph;
  *reader = scan;
  return true_v;
malformed:
  return graph_error(error, "Malformed or unsupported animation graph JSON v1");
}

bool8_t vkr_animation_graph_write_json(VkrJsonWriter *writer,
                                       const VkrAnimationGraph *graph) {
  if (!writer || !vkr_animation_graph_validate(graph, NULL, NULL)) {
    return false_v;
  }
  vkr_json_writer_begin_object(writer);
  vkr_json_writer_name(writer, string8_lit("version"));
  vkr_json_writer_u64(writer, 1u);
  vkr_json_writer_name(writer, string8_lit("cycle"));
  vkr_json_writer_f64(writer, graph->cycle_seconds);
  vkr_json_writer_name(writer, string8_lit("root"));
  vkr_json_writer_u64(writer, graph->root);
  vkr_json_writer_name(writer, string8_lit("initial"));
  vkr_json_writer_u64(writer, graph->initial_state);
  vkr_json_writer_name(writer, string8_lit("parameters"));
  vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; i < graph->parameter_count; ++i) {
    vkr_json_writer_f64(writer, graph->parameters[i]);
  }
  vkr_json_writer_end_array(writer);
  vkr_json_writer_name(writer, string8_lit("nodes"));
  vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    const VkrAnimationGraphNode *node = &graph->nodes[i];
    vkr_json_writer_begin_array(writer);
    vkr_json_writer_u64(writer, node->kind);
    vkr_json_writer_u64(writer, node->clip);
    vkr_json_writer_u64(writer, node->inputs[0]);
    vkr_json_writer_u64(writer, node->inputs[1]);
    vkr_json_writer_u64(writer, node->parameter_x);
    vkr_json_writer_u64(writer, node->parameter_y);
    vkr_json_writer_begin_array(writer);
    for (uint32_t j = 0; j < node->sample_count; ++j) {
      vkr_json_writer_begin_array(writer);
      vkr_json_writer_u64(writer, node->samples[j].clip);
      vkr_json_writer_f64(writer, node->samples[j].x);
      vkr_json_writer_f64(writer, node->samples[j].y);
      vkr_json_writer_end_array(writer);
    }
    vkr_json_writer_end_array(writer);
    vkr_json_writer_begin_array(writer);
    for (uint32_t j = 0; j < node->triangle_count; ++j) {
      vkr_json_writer_begin_array(writer);
      for (uint32_t k = 0; k < 3u; ++k) {
        vkr_json_writer_u64(writer, node->triangles[j].samples[k]);
      }
      vkr_json_writer_end_array(writer);
    }
    vkr_json_writer_end_array(writer);
    vkr_json_writer_end_array(writer);
  }
  vkr_json_writer_end_array(writer);
  vkr_json_writer_name(writer, string8_lit("states"));
  vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; i < graph->state_count; ++i) {
    vkr_json_writer_begin_array(writer);
    vkr_json_writer_u64(writer, graph->states[i].root);
    vkr_json_writer_f64(writer, graph->states[i].cycle_seconds);
    vkr_json_writer_end_array(writer);
  }
  vkr_json_writer_end_array(writer);
  vkr_json_writer_name(writer, string8_lit("transitions"));
  vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; i < graph->transition_count; ++i) {
    const VkrAnimationGraphTransition *t = &graph->transitions[i];
    vkr_json_writer_begin_array(writer);
    vkr_json_writer_u64(writer, t->from);
    vkr_json_writer_u64(writer, t->to);
    vkr_json_writer_u64(writer, t->parameter);
    vkr_json_writer_u64(writer, t->comparison);
    vkr_json_writer_f64(writer, t->threshold);
    vkr_json_writer_f64(writer, t->duration);
    vkr_json_writer_f64(writer, t->exit_time);
    vkr_json_writer_end_array(writer);
  }
  vkr_json_writer_end_array(writer);
  vkr_json_writer_end_object(writer);
  return !writer->failed;
}
