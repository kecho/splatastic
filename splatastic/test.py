from .  import native as n
from . import scene_loader
import coalpy.gpu as g

g.init()


def testIOResolve(fileStr):
    print ("[testIOResolve begin]")
    print ("\tloading file " + fileStr)
    request = n.SceneAsyncRequest(file = fileStr)
    request.resolve()
    (status, msg) = request.status()
    print("\t"+("Success" if scene_loader.SuccessFinish  else "Failed")+ msg)
    print ("[testIOResolve end]")

def testIOStreaming(fileStr):
    print ("[testIOStreaming begin]")
    print ("\tloading file " + fileStr)
    request = n.SceneAsyncRequest(file = fileStr)
    (status, msg) = request.status()
    ii = 0
    while status == scene_loader.Reading:
        (bytes_read, total_bytes) = request.ioProgress()
        if total_bytes == 0:
            (status, msg) = request.status()
            continue
        elif (ii % 40000) == 0:
            print ("\t"+str((bytes_read/total_bytes) * 100))
        (status, msg) = request.status()
        ii += 1

    print("\t"+("Success" if scene_loader.SuccessFinish  else "Failed")+ msg)
    print ("[testIOStreaming end]")


if __name__=="__main__":
    print ("Native init")
    n.init()

    fileStr = "test_data/train.ply"
    testIOResolve(fileStr)
    testIOStreaming(fileStr)
    
    print ("Native shutdown")
    n.shutdown()
