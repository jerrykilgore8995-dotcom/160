#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mkii::native {

// Choose and factor the symbolic pattern at prepareToPlay. The audio thread
// reuses a fixed elimination schedule with no searches, allocation or fill-in.
class Sparse {
public:
    void resize(int n) {
        size = n;
        lookup.assign(n * n, -1);
        dense.assign(n * n, 0);
        rhs.assign(n, 0);
        x.assign(n, 0);
        ready = false;
    }

    void add(int row, int column, double value) {
        if (row < 0 || column < 0) return;
        const int offset = row * size + column;
        if (!ready) {
            lookup[offset] = 0;
            dense[offset] += value;
        } else {
            const int slot = lookup[offset];
            if (slot >= 0) entries[slot] += value;
        }
    }

    bool prepare() {
        rows.clear();
        updates.clear();
        std::vector<bool> liveRows(size, true), liveColumns(size, true);
        for (int step = 0; step < size; ++step) {
            std::vector<int> rowCount(size, 0), colCount(size, 0);
            std::vector<double> colMax(size, 0), rowMax(size, 0);
            for (int row = 0; row < size; ++row) if (liveRows[row]) {
                for (int col = 0; col < size; ++col) if (liveColumns[col] && has(row,col)) {
                    ++rowCount[row];
                    ++colCount[col];
                    colMax[col] = std::max(colMax[col], std::abs(v(row,col)));
                    rowMax[row] = std::max(rowMax[row], std::abs(v(row,col)));
                }
            }
            int pr = -1, pc = -1, best = size * size;
            double tie = -1.0;
            for (int row = 0; row < size; ++row) if (liveRows[row]) {
                for (int col = 0; col < size; ++col) if (liveColumns[col] && has(row,col)) {
                    const double pivot = std::abs(v(row,col));
                    if (pivot < 1e-17 || pivot < 0.01 * colMax[col]) continue;
                    const int cost = (rowCount[row]-1) * (colCount[col]-1);
                    const double scaled = pivot / (rowMax[row] + 1e-30);
                    if (cost < best || (cost == best && scaled > tie)) {
                        best = cost;
                        tie = scaled;
                        pr = row;
                        pc = col;
                    }
                }
            }
            if (pr < 0) return false;
            liveRows[pr] = liveColumns[pc] = false;
            Row s{};
            s.row = pr;
            s.column = pc;
            for (int col = 0; col < size; ++col)
                if (liveColumns[col] && has(pr,col)) s.upper.push_back({col, 0});
            for (int row = 0; row < size; ++row) {
                if (!liveRows[row] || !has(row,pc)) continue;
                s.lower.push_back({row, 0});
                const double factor = v(row,pc)/v(pr,pc);
                for (const auto& upper : s.upper) {
                    lookup[row*size + upper.index] = 0;
                    v(row,upper.index) -= factor * v(pr,upper.index);
                }
            }
            rows.push_back(std::move(s));
        }
        int count = 0;
        for (auto& slot : lookup) if (slot >= 0) slot = count++;
        entries.assign(count+1, 0);
        plan.clear();
        uppers.clear();
        lowers.clear();
        for (auto& row : rows) {
            const int upperBegin=static_cast<int>(uppers.size());
            const int lowerBegin=static_cast<int>(lowers.size());
            for (auto& upper : row.upper) {
                upper.slot = at(row.row,upper.index);
                uppers.push_back(upper);
            }
            for (auto& lower : row.lower) {
                lower.slot = at(lower.index,row.column);
                lowers.push_back(lower);
                for (const auto& upper : row.upper)
                    updates.push_back(at(lower.index,upper.index));
            }
            plan.push_back({row.row,row.column,at(row.row,row.column),upperBegin,
                            static_cast<int>(uppers.size()),lowerBegin,static_cast<int>(lowers.size())});
        }
        rows.clear();
        dense.clear();
        ready = true;
        return true;
    }

    void cacheLinearPart() { fixed = entries; }

    void startIteration(const std::vector<double>& initial) {
        std::copy(fixed.begin(),fixed.end(),entries.begin());
        std::copy(initial.begin(),initial.end(),rhs.begin());
    }

    void clear() {
        std::fill(entries.begin(), entries.end(), 0.0);
        std::fill(rhs.begin(), rhs.end(), 0.0);
    }

    bool solve() {
        int offset=0;
        const auto* upper=uppers.data();
        const auto* lower=lowers.data();
        auto* values=entries.data();
        auto* vector=rhs.data();
        for (const auto& s : plan) {
            const double pivot=values[s.pivot];
            if (std::abs(pivot)<1e-17 || !std::isfinite(pivot)) return false;
            const double inv=1.0/pivot;
            values[s.pivot]=inv;
            for (int j=s.lowerBegin;j<s.lowerEnd;++j) {
                const double factor=values[lower[j].slot]*inv;
                vector[lower[j].index]-=factor*vector[s.row];
                for (int k=s.upperBegin;k<s.upperEnd;++k)
                    values[updates[offset++]]-=factor*values[upper[k].slot];
            }
        }
        for (auto s=plan.rbegin();s!=plan.rend();++s) {
            double y=vector[s->row];
            for (int k=s->upperBegin;k<s->upperEnd;++k)
                y-=values[upper[k].slot]*x[upper[k].index];
            x[s->column]=y*values[s->pivot];
            if (!std::isfinite(x[s->column])) return false;
        }
        return true;
    }

    int slot(int row,int col) const {
        if (row<0 || col<0) return static_cast<int>(entries.size()-1);
        const int i=at(row,col);
        return i<0?static_cast<int>(entries.size()-1):i;
    }
    void addSlot(int slot,double value) { entries[slot]+=value; }

    int operationCount() const { return static_cast<int>(updates.size()); }
    std::vector<double> rhs, x;

private:
    struct Terminal { int index, slot; };
    struct Row {
        int row, column;
        std::vector<Terminal> upper, lower;
    };
    int at(int row, int col) const { return lookup[row*size+col]; }
    bool has(int row, int col) const { return at(row,col) >= 0; }
    double& v(int row, int col) { return dense[row*size+col]; }

    int size = 0;
    bool ready = false;
    std::vector<int> lookup, updates;
    std::vector<Row> rows;
    struct Pivot { int row,column,pivot,upperBegin,upperEnd,lowerBegin,lowerEnd; };
    std::vector<Pivot> plan;
    std::vector<Terminal> uppers,lowers;
    std::vector<double> dense, entries, fixed;
};
}
