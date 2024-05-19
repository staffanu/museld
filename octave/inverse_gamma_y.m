function x=inverse_gamma_y(y)

min_value = 16;
max_value = 239;
value_range = max_value - min_value;

normalized = (max(min_value, min(max_value, y)) - min_value) / value_range;
x = (0.6 * normalized + 0.4) * normalized;
x = x * value_range + min_value;

