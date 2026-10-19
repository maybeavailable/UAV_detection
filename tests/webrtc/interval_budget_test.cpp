#include"webrtc/congestion_control/interval_budget.h"

#include<cmath>
#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
int main()
{
    try
    {
        IntervalBudget budget(800000);
        CHECK(budget.maxBytesInBudget() == 50000 && budget.bytesRemaining() == 0);
        budget.increaseBudget(5ms); CHECK(budget.bytesRemaining() == 500);
        budget.increaseBudget(5ms); CHECK(budget.bytesRemaining() == 500); // unused credit is replaced
        budget.useBudget(1200); CHECK(budget.signedBytesRemaining() == -700 && budget.bytesRemaining() == 0);
        budget.increaseBudget(5ms); CHECK(budget.signedBytesRemaining() == -200);
        budget.increaseBudget(5ms); CHECK(budget.bytesRemaining() == 300);
        CHECK(std::abs(budget.budgetRatio() - 0.006) < 1e-9);
        budget.useBudget(std::numeric_limits<std::size_t>::max()); CHECK(budget.signedBytesRemaining() == -50000);
        CHECK(budget.setTargetRate(80000) && budget.signedBytesRemaining() == -5000);
        CHECK(!budget.setTargetRate(IntervalBudget::MaxRateBps + 1) && budget.targetRateBps() == 80000);
        budget.increaseBudget(24h); CHECK(budget.bytesRemaining() == 5000);
        budget.useBudget(10000); CHECK(budget.signedBytesRemaining() == -5000);
        budget.increaseBudget(500ms); CHECK(budget.bytesRemaining() == 0);
        budget.increaseBudget(500ms); CHECK(budget.bytesRemaining() == 5000);
        budget.discardPositive(); CHECK(budget.bytesRemaining() == 0);
        budget.useBudget(1000); CHECK(budget.setTargetRate(0) && budget.signedBytesRemaining() == 0);
        budget.increaseBudget(1s); CHECK(budget.budgetRatio() == 0);

        IntervalBudget accumulated(8000, true);
        accumulated.increaseBudget(100ms); accumulated.increaseBudget(100ms); CHECK(accumulated.bytesRemaining() == 200);
        accumulated.increaseBudget(1s); CHECK(accumulated.bytesRemaining() == 500);
        accumulated.useBudget(800); CHECK(accumulated.signedBytesRemaining() == -300);
        accumulated.reset(); CHECK(accumulated.signedBytesRemaining() == 0 && accumulated.targetRateBps() == 8000);

        // A non-kbps rate and sub-byte processing intervals must not drift down to zero.
        IntervalBudget fractional(1001, true);
        for (unsigned i = 0; i < 1000; ++i) fractional.increaseBudget(1us);
        CHECK(fractional.bytesRemaining() == 0);
        for (unsigned i = 0; i < 7; ++i) fractional.increaseBudget(1ms);
        CHECK(fractional.bytesRemaining() == 1);
        fractional.increaseBudget(-1ms); CHECK(fractional.bytesRemaining() == 1);
        IntervalBudget largest(IntervalBudget::MaxRateBps, true);
        largest.increaseBudget(std::chrono::microseconds::max()); CHECK(largest.bytesRemaining() == 62500000);

        // ALR requires the original whole-byte truncation per interval, while
        // the pacer's default continues carrying fractional bytes.
        IntervalBudget truncated(195000, true, IntervalBudget::Rounding::TruncateBytes);
        IntervalBudget carried(195000, true);
        for (int i = 0; i < 4; ++i) { truncated.increaseBudget(10ms); carried.increaseBudget(10ms); }
        CHECK(truncated.bytesRemaining() == 972 && carried.bytesRemaining() == 975);
        truncated.useBudget(1000); CHECK(truncated.signedBytesRemaining() == -28);
        truncated.increaseBudget(10ms); CHECK(truncated.bytesRemaining() == 215);
        truncated.reset(); CHECK(truncated.bytesRemaining() == 0);
        CHECK(truncated.setTargetRate(8000)); truncated.increaseBudget(24h);
        CHECK(truncated.bytesRemaining() == 500);
        std::cout << "PASS: interval budgets, overdraft, underuse, rate changes and integer limits\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
