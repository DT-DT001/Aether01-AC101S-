#pragma once
#define ESP_RETURN_ON_FALSE(value,error,...) do { if (!(value)) return (error); } while (0)
