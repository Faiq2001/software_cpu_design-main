#include <stdio.h>

/* Recursive factorial used only for the C vs Tiny16 stack-layout comparison. */
static long long factorialByRecursion(int n) {
    if (n == 0 || n == 1)
        return 1;
    return (long long)n * factorialByRecursion(n - 1);
}

int main(void) {
    int inputValue;

    printf("Enter a number: ");
    scanf("%d", &inputValue);

    if (inputValue < 0) {
        printf("Factorial is not defined for negative numbers.\n");
    } else {
        long long factorialResult = factorialByRecursion(inputValue);
        printf("Factorial of %d = %lld\n", inputValue, factorialResult);
    }

    return 0;
}
