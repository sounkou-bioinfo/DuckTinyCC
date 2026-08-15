int sum_i32(const int *x, int n) {
    int i;
    int s = 0;
    for (i = 0; i < n; ++i)
        s += x[i];
    return s;
}
