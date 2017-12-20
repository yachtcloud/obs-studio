import json
import sys

from pprint import pprint

sceneName = sys.argv[1]
ffInput = sys.argv[2]

try:
    data = json.load(open('/home/ycadmin/.config/obs-studio/basic/scenes/'+sceneName+'.json', 'r'))


    def get_tile_name_by_url (url):
        for source in data['sources']:
            if ('settings' in source and 'ffinput' in source['settings'] and source['settings']['ffinput'] == url):
                return source['name']
            if ('settings' in source and 'input' in source['settings'] and source['settings']['input'] == url):
                return source['name']

    tile_name = get_tile_name_by_url(ffInput)

    for source in data['sources']:
        if (source["id"] == "scene"):
            for item in source['settings']['items']:
                if (item['name'] == tile_name):
                    print(str(int(item["bounds"]["x"]))+":"+str(int(item["bounds"]["y"])))

except:
    pass
    #print "Unexpected error:", sys.exc_info()[0]
