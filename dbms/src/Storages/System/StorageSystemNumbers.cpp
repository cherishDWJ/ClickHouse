#include <Common/Exception.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataStreams/IBlockInputStream.h>
#include <DataStreams/LimitBlockInputStream.h>
#include <Storages/System/StorageSystemNumbers.h>

#include <Processors/Sources/SourceWithProgress.h>
#include <Processors/Pipe.h>
#include <Processors/LimitTransform.h>

namespace DB
{

namespace
{

class NumbersSource : public SourceWithProgress
{
public:
    NumbersSource(UInt64 block_size_, UInt64 offset_, UInt64 step_)
        : SourceWithProgress(createHeader()), block_size(block_size_), next(offset_), step(step_) {}

    String getName() const override { return "Numbers"; }

protected:
    Chunk generate() override
    {
        auto column = ColumnUInt64::create(block_size);
        ColumnUInt64::Container & vec = column->getData();

        size_t curr = next;     /// The local variable for some reason works faster (>20%) than member of class.
        UInt64 * pos = vec.data(); /// This also accelerates the code.
        UInt64 * end = &vec[block_size];
        while (pos < end)
            *pos++ = curr++;

        next += step;

        progress({column->size(), column->byteSize()});

        return { Columns {std::move(column)}, block_size };
    }

private:
    UInt64 block_size;
    UInt64 next;
    UInt64 step;

    static Block createHeader()
    {
        return { ColumnWithTypeAndName(ColumnUInt64::create(), std::make_shared<DataTypeUInt64>(), "number") };
    }
};


struct NumbersMultiThreadedState
{
    std::atomic<UInt64> counter;
    explicit NumbersMultiThreadedState(UInt64 offset) : counter(offset) {}
};

using NumbersMultiThreadedStatePtr = std::shared_ptr<NumbersMultiThreadedState>;

class NumbersMultiThreadedSource : public SourceWithProgress
{
public:
    NumbersMultiThreadedSource(NumbersMultiThreadedStatePtr state_, UInt64 block_size_, UInt64 max_counter_)
        : SourceWithProgress(createHeader())
        , state(std::move(state_))
        , block_size(block_size_)
        , max_counter(max_counter_) {}

    String getName() const override { return "NumbersMt"; }

protected:
    Chunk generate() override
    {
        if (block_size == 0)
            return {};

        UInt64 curr = state->counter.fetch_add(block_size, std::memory_order_acquire);

        if (curr >= max_counter)
            return {};

        if (curr + block_size > max_counter)
            block_size = max_counter - curr;

        auto column = ColumnUInt64::create(block_size);
        ColumnUInt64::Container & vec = column->getData();

        UInt64 * pos = vec.data();
        UInt64 * end = &vec[block_size];
        while (pos < end)
            *pos++ = curr++;

        progress({column->size(), column->byteSize()});

        return { Columns {std::move(column)}, block_size };
    }

private:
    NumbersMultiThreadedStatePtr state;

    UInt64 block_size;
    UInt64 max_counter;

    Block createHeader() const
    {
        return { ColumnWithTypeAndName(ColumnUInt64::create(), std::make_shared<DataTypeUInt64>(), "number") };
    }
};

}


StorageSystemNumbers::StorageSystemNumbers(const std::string & name_, bool multithreaded_, std::optional<UInt64> limit_, UInt64 offset_, bool even_distribution_)
    : IStorage({"system", name_}), multithreaded(multithreaded_), even_distribution(even_distribution_), limit(limit_), offset(offset_)
{
    setColumns(ColumnsDescription({{"number", std::make_shared<DataTypeUInt64>()}}));
}

Pipes StorageSystemNumbers::readWithProcessors(
    const Names & column_names,
    const SelectQueryInfo &,
    const Context & /*context*/,
    QueryProcessingStage::Enum /*processed_stage*/,
    size_t max_block_size,
    unsigned num_streams)
{
    check(column_names);

    if (limit && *limit < max_block_size)
    {
        max_block_size = static_cast<size_t>(*limit);
        multithreaded = false;
    }

    if (!multithreaded)
        num_streams = 1;

    Pipes res;
    res.reserve(num_streams);

    if (num_streams > 1 && !even_distribution && *limit)
    {
        auto state = std::make_shared<NumbersMultiThreadedState>(offset);
        UInt64 max_counter = offset + *limit;

        for (size_t i = 0; i < num_streams; ++i)
            res.emplace_back(std::make_shared<NumbersMultiThreadedSource>(state, max_block_size, max_counter));

        return res;
    }

    for (size_t i = 0; i < num_streams; ++i)
    {
        auto source = std::make_shared<NumbersSource>(max_block_size, offset + i * max_block_size, num_streams * max_block_size);

        if (limit && i == 0)
            source->addTotalRowsApprox(*limit);

        res.emplace_back(std::move(source));

        if (limit)
        {
            /// This formula is how to split 'limit' elements to 'num_streams' chunks almost uniformly.
            res.back().addSimpleTransform(std::make_shared<LimitTransform>(
                    res.back().getHeader(), *limit * (i + 1) / num_streams - *limit * i / num_streams, 0, false));
        }
    }

    return res;
}

}
