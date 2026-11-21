#pragma once
#define ESP_RETURN_ON_FALSE(check, err, ...) do { if (!(check)) return (err); } while (0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int result_ = (expr); if (result_) return result_; } while (0)
#define ESP_GOTO_ON_FALSE(check, err, label, ...) do { if (!(check)) { ret = (err); goto label; } } while (0)
#define ESP_GOTO_ON_ERROR(expr, label, ...) do { ret = (expr); if (ret) goto label; } while (0)
