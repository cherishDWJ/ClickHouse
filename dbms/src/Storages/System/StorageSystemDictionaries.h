#pragma once

#include <ext/shared_ptr_helper.h>
#include <Storages/System/IStorageSystemOneBlock.h>


namespace DB
{

class Context;


class StorageSystemDictionaries final : public StorageHelper<StorageSystemDictionaries>, public IStorageSystemOneBlock<StorageSystemDictionaries>
{
    friend struct StorageHelper<StorageSystemDictionaries>;
public:
    std::string getName() const override { return "SystemDictionaries"; }

    static NamesAndTypesList getNamesAndTypes();

protected:
    using IStorageSystemOneBlock::IStorageSystemOneBlock;

    void fillData(MutableColumns & res_columns, const Context & context, const SelectQueryInfo & query_info) const override;
};

}
