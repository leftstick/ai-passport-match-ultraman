idf_component_register(
    SRCS "main.c"
         "app/battle_app.c"
         "core/anim.c"
         "core/battle.c"
         "core/campaign.c"
         "core/math_question.c"
         "audio/audio_player.c"
         "audio/sfx.c"
         "audio/sfx_extra.c"
         "assets/sprites.c"
         "fonts/app_font_12.c"
         "fonts/app_font_14.c"
         "fonts/app_font_20.c"
         "fonts/app_font_28.c"
    INCLUDE_DIRS "." "app" "core" "audio" "assets"
    REQUIRES bsp esp_timer nvs_flash
)
