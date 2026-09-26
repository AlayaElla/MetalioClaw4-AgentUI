#define main calculator_app_entry
#include "../../external_apps/examples/calculator/main/calculator.cc"
#undef main
#include <cassert>
#include <string>

int main() {
    metalio_app_host_api_t api{};
    metalio_app_launch_context_t launch{};
    api.set_label_text = [](void*, metalio_app_widget_t, const char*) { return 0; };
    api.set_label_color = [](void*, metalio_app_widget_t, uint32_t) { return 0; };
    api.set_label_font = [](void*, metalio_app_widget_t, metalio_app_font_t) { return 0; };
    api.set_widget_visible = [](void*, metalio_app_widget_t, uint8_t) { return 0; };
    api.set_action_segment_selected = [](void*, metalio_app_widget_t, uint8_t) { return 0; };
    s_app.api = &api; s_app.launch = &launch;
    auto invoke = [](const char* action, const char* arguments) {
        metalio_app_ai_action_request_t request{};
        request.id = action; request.arguments_json = arguments;
        metalio_app_ai_action_result_t result{};
        assert(AiAction(nullptr, &request, &result) == 0);
        return result;
    };
    auto result = invoke("com.metalio.calculator.evaluate", "{\"expression\":\"(2+3)*4\"}");
    assert(result.status == METALIO_APP_AI_ACTION_SUCCEEDED);
    // Copy after returning through another frame, as the ELF host does.
    assert(std::string(result.result_json) == "{\"value\":20}");
    result = invoke("com.metalio.calculator.clear", "{}");
    assert(result.status == METALIO_APP_AI_ACTION_SUCCEEDED);
    assert(std::string(result.result_json) == "{\"value\":0}");
    result = invoke("com.metalio.calculator.mode", "{\"mode\":\"scientific\"}");
    assert(result.status == METALIO_APP_AI_ACTION_SUCCEEDED);
    assert(std::string(result.result_json) == "{\"mode\":\"scientific\"}");
    result = invoke("com.metalio.calculator.evaluate", "{\"expression\":\"1/0\"}");
    assert(result.status == METALIO_APP_AI_ACTION_FAILED);
    result = invoke("com.metalio.calculator.evaluate", "{\"expression\":\"\\u0000\"}");
    assert(result.status == METALIO_APP_AI_ACTION_FAILED);
    puts("Calculator production AI callback: evaluate, clear, mode, errors and result lifetime passed");
}
