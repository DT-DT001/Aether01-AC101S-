#pragma once
#define ESP_RETURN_ON_FALSE(check, error, ...) do { if (!(check)) return error; } while (0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int e_ = expr; if (e_) return e_; } while (0)
