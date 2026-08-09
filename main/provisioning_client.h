#ifndef _PROVISIONING_CLIENT_H
#define _PROVISIONING_CLIENT_H

#include <memory>
#include <string>

#include <esp_err.h>
#include "board.h"

class ProvisioningClient {
public:
    ProvisioningClient();
    ~ProvisioningClient();

    esp_err_t FetchConfiguration();
    esp_err_t Activate();
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool HasServerTime() { return has_server_time_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    std::string GetEndpointUrl();

private:
    std::string activation_message_;
    std::string activation_code_;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string activation_challenge_;
    std::string serial_number_;
    int activation_timeout_ms_ = 30000;

    std::string GetActivationPayload();
    std::unique_ptr<Http> SetupHttp();
};

#endif // _PROVISIONING_CLIENT_H
