      
def UniqueInsert(list_):        #function to insert the data
     if(inmongo(list_)==False) :
        collection.insert_one(list_)
        return True
     return False    
    
def inmongo(list_):             #function to check if the data is already in the database
    search=collection.find_one({"title": list_["title"]})
    return search is not None