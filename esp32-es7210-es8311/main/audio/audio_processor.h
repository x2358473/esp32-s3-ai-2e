#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <model_path.h>

struct AudioCodec;
typedef struct AudioCodec AudioCodec;

typedef void (*audio_processor_output_cb_t)(const int16_t* data, size_t samples, void* user_ctx);
typedef void (*audio_processor_vad_cb_t)(bool speaking, void* user_ctx);

typedef struct AudioProcessor {
    void* impl;
    void (*destroy)(struct AudioProcessor* processor);
    void (*initialize)(struct AudioProcessor* processor, AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list);
    void (*feed)(struct AudioProcessor* processor, const int16_t* data, size_t samples);
    void (*start)(struct AudioProcessor* processor);
    void (*stop)(struct AudioProcessor* processor);
    bool (*is_running)(struct AudioProcessor* processor);
    void (*on_output)(struct AudioProcessor* processor, audio_processor_output_cb_t callback, void* user_ctx);
    void (*on_vad_state_change)(struct AudioProcessor* processor, audio_processor_vad_cb_t callback, void* user_ctx);
    size_t (*get_feed_size)(struct AudioProcessor* processor);
    void (*enable_device_aec)(struct AudioProcessor* processor, bool enable);
} AudioProcessor;

#endif
