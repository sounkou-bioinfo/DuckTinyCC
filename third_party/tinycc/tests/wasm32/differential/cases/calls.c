int diff_call_leaf(int a, int b)
{
    return a * b + 7;
}

int diff_call_chain(int a, int b)
{
    return diff_call_leaf(a + 1, b - 1) - diff_call_leaf(2, 3);
}

double diff_call_f64(double a, double b, double c)
{
    return (a + b) * c;
}
