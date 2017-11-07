#include <libudev.h>
#include <mntent.h>
#include <pthread.h>
#include <unistd.h>

#include "detection.h"
#include "deviceList.h"

using namespace std;



/**********************************
 * Local defines
 **********************************/
#define DEVICE_ACTION_ADDED             "add"
#define DEVICE_ACTION_REMOVED           "remove"

#define DEVICE_TYPE_DEVICE              "usb_device"
#define DEVICE_TYPE_PARTITION           "partition"

#define DEVICE_PROPERTY_NAME            "ID_MODEL"
#define DEVICE_PROPERTY_SERIAL          "ID_SERIAL_SHORT"
#define DEVICE_PROPERTY_VENDOR          "ID_VENDOR"


/**********************************
 * Local typedefs
 **********************************/



/**********************************
 * Local Variables
 **********************************/
ListResultItem_t*             currentItem;

bool                         isAdded;
struct udev*                 udev;
struct udev_enumerate*       enumerate;
struct udev_list_entry*      devices;
struct udev_list_entry*      dev_list_entry;
struct udev_device*          dev;

struct udev_monitor*         mon;
int                          fd;

pthread_t       thread;
pthread_mutex_t notify_mutex;
pthread_cond_t  notifyNewDevice;
pthread_cond_t  notifyDeviceHandled;

bool newDeviceAvailable = false;
bool deviceHandled      = true;

bool isRunning          = false;
/**********************************
 * Local Helper Functions protoypes
 **********************************/
void  BuildInitialDeviceList();

void* ThreadFunc(void* ptr);
void  WaitForDeviceHandled();
void  SignalDeviceHandled();
void  WaitForNewDevice();
void  SignalDeviceAvailable();

/**********************************
 * Public Functions
 **********************************/
void NotifyAsync(uv_work_t* req)
{
    WaitForNewDevice();
}


void NotifyFinished(uv_work_t* req)
{
    if (isRunning)
    {
        if (isAdded)
        {
            NotifyAdded(currentItem);
        }

        else
        {
            NotifyRemoved(currentItem);
        }
    }

    // Delete Item in case of removal
    if (isAdded == false)
    {
        delete currentItem;
    }

    SignalDeviceHandled();
    if (isRunning)
	uv_queue_work(uv_default_loop(), req, NotifyAsync, (uv_after_work_cb)NotifyFinished);
}

void Start()
{
    NotifyLog("Start");
    isRunning = true;
}

void Stop()
{
    isRunning = false;
    pthread_mutex_lock(&notify_mutex);
    pthread_cond_signal(&notifyNewDevice);
    pthread_mutex_unlock(&notify_mutex);
    
    pthread_mutex_lock(&notify_mutex);
    pthread_cond_signal(&notifyDeviceHandled);
    pthread_mutex_unlock(&notify_mutex);
}

void GetInitMountPath(struct udev_device* dev, ListResultItem_t* item)
{
    struct mntent *mnt;
    FILE          *fp      = NULL;
    const char    *devNode = udev_device_get_devnode(dev);

    // TODO: find a better way to replace waiting for a second
    sleep(1);
    if ((fp = setmntent("/proc/mounts", "r")) == NULL)
    {
        //TODO: sent error to js layer
        //NanThrowError("Can't open mounted filesystems\n");
        printf("Can't open mounted filesystems\n");
        return;
    }

    while ((mnt = getmntent(fp)))
    {
        if (mnt->mnt_fsname == strstr(mnt->mnt_fsname, devNode))//(!strcmp(mnt->mnt_fsname, devNode))
        {
            item->mountPath = mnt->mnt_dir;
        }
    }

    /* close file for describing the mounted filesystems */
    endmntent(fp);

    SignalDeviceAvailable();
}

void GetMountPath(struct udev_device* dev, ListResultItem_t* item)
{
    struct mntent *mnt;
    FILE          *fp      = NULL;
    const char    *devNode = udev_device_get_devnode(dev);
    
    std::string s(devNode);
    item->devNode = s;

    // TODO: find a better way to replace waiting for a second
    sleep(1);
    if ((fp = setmntent("/proc/mounts", "r")) == NULL)
    {
        //TODO: sent error to js layer
        //NanThrowError("Can't open mounted filesystems\n");
        printf("Can't open mounted filesystems\n");
        return;
    }

    while ((mnt = getmntent(fp)))
    {
        if (strcmp(mnt->mnt_fsname, devNode) == 0)
        {
            item->mountPath = mnt->mnt_dir;
        }
        //else {
        //    printf("mnt_fsname %s\n", mnt->mnt_fsname);
        //    printf("mnt->mnt_dir %s\n", mnt->mnt_dir);
        //    printf("devNode %s\n", devNode);
        //}
    }

    /* close file for describing the mounted filesystems */
    endmntent(fp);

    SignalDeviceAvailable();
}

void GetMountPath2(struct udev_device* dev, ListResultItem_t* item)
{
    struct mntent *mnt;
    FILE          *fp      = NULL;
    const char    *devNode = udev_device_get_devnode(dev);

    std::string s(devNode);
    item->devNode = s;

    // TODO: find a better way to replace waiting for a second
    sleep(1);
    if ((fp = setmntent("/proc/mounts", "r")) == NULL)
    {
        //TODO: sent error to js layer
        //NanThrowError("Can't open mounted filesystems\n");
        printf("Can't open mounted filesystems\n");
        return;
    }

    while ((mnt = getmntent(fp)))
    {
        if (!strcmp(mnt->mnt_fsname, devNode))
        {
            item->mountPath = mnt->mnt_dir;
        } else {
            //printf("mnt_fsname %s\n", mnt->mnt_fsname);
            //printf("mnt->mnt_dir %s\n", mnt->mnt_dir);
        }
    }

    /* close file for describing the mounted filesystems */
    endmntent(fp);

    SignalDeviceAvailable();
}

static struct udev_device* get_child(struct udev* udev, struct udev_device* parent, const char* subsystem) {
  struct udev_device* child = NULL;
  struct udev_enumerate *enumerate = udev_enumerate_new(udev);

  udev_enumerate_add_match_parent(enumerate, parent);
  udev_enumerate_add_match_subsystem(enumerate, subsystem);
  udev_enumerate_scan_devices(enumerate);

  struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
  struct udev_list_entry *entry;

  udev_list_entry_foreach(entry, devices) {
    const char *path = udev_list_entry_get_name(entry);
    child = udev_device_new_from_syspath(udev, path);
    break;
  }

  udev_enumerate_unref(enumerate);
  return child;
}

static void enumerate_usb_mass_storage(struct udev* udev) {
  struct udev_enumerate* enumerate = udev_enumerate_new(udev);

  udev_enumerate_add_match_subsystem(enumerate, "scsi");
  udev_enumerate_add_match_property(enumerate, "DEVTYPE", "scsi_device");
  udev_enumerate_scan_devices(enumerate);

  struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
  struct udev_list_entry *entry;

  udev_list_entry_foreach(entry, devices) {
    const char* path = udev_list_entry_get_name(entry);
    //printf("   Path absolute: %s\n", path);
    struct udev_device* scsi = udev_device_new_from_syspath(udev, path);

    struct udev_device* block = get_child(udev, scsi, "block");
    struct udev_device* scsi_disk = get_child(udev, scsi, "scsi_disk");

    struct udev_device* usb
      = udev_device_get_parent_with_subsystem_devtype(
          scsi, "usb", "usb_device");

    if (block && scsi_disk && usb) {
        const char  *devNode  = udev_device_get_devnode(block);
    	const char* idVendor = udev_device_get_sysattr_value(usb, "idVendor");
    	const char* idProduct = udev_device_get_sysattr_value(usb, "idProduct");
    	//const char* vendor = udev_device_get_sysattr_value(scsi, "vendor");
    
    	/*
        printf("block = %s, usb = %s:%s, scsi = %s\n",
          devNode,
          idVendor,
          idProduct,
          vendor);
        */
    
	DeviceItem_t* item = new DeviceItem_t();
	item->deviceParams.devNode = devNode;
	item->deviceParams.vendorId = strtol (idVendor, NULL, 16);
	item->deviceParams.productId = strtol (idProduct, NULL, 16);


	if (udev_device_get_sysattr_value(usb, "product") != NULL)
	{
	    item->deviceParams.deviceName = udev_device_get_sysattr_value(usb, "product");
	}

	if (udev_device_get_sysattr_value(usb, "manufacturer") != NULL)
	{
	    item->deviceParams.manufacturer = udev_device_get_sysattr_value(usb, "manufacturer");
	}

	if (udev_device_get_sysattr_value(usb, "serial") != NULL)
	{
	    item->deviceParams.serialNumber = udev_device_get_sysattr_value(usb, "serial");
	}

	item->deviceParams.deviceAddress = 0;
	item->deviceParams.locationId = 0;
		

	GetInitMountPath(block, &item->deviceParams);

	item->deviceState = DeviceState_Connect;

	AddItemToList((char *)devNode, item);
    }    

    if (block)
      udev_device_unref(block);

    if (scsi_disk)
      udev_device_unref(scsi_disk);

    udev_device_unref(scsi);
  }

  udev_enumerate_unref(enumerate);
}

void InitDetection()
{
    NotifyLog("InitDetection");
    
    /* Create the udev object */
    udev = udev_new();

    if (!udev)
    {
        printf("Can't create udev\n");
        return;
    }

    /* Set up a monitor to monitor devices */
    mon = udev_monitor_new_from_netlink(udev, "udev");
    //udev_monitor_filter_add_match_subsystem_devtype(mon, "usb", "usb_device");
    
    udev_monitor_filter_add_match_subsystem_devtype(mon, "block", NULL);
    udev_monitor_filter_add_match_subsystem_devtype(mon, "usb","usb_device");
    
    udev_monitor_enable_receiving(mon);

    /* Get the file descriptor (fd) for the monitor.
       This fd will get passed to select() */
    fd = udev_monitor_get_fd(mon);


    enumerate_usb_mass_storage(udev);
    
    //BuildInitialDeviceList();

    pthread_mutex_init(&notify_mutex, NULL);
    pthread_cond_init(&notifyNewDevice, NULL);
    pthread_cond_init(&notifyDeviceHandled, NULL);       
    
    Start();

    uv_work_t *req = new uv_work_t();
    uv_queue_work(uv_default_loop(), req, NotifyAsync, (uv_after_work_cb)NotifyFinished);
    
    pthread_create(&thread, NULL, ThreadFunc, NULL);    
}


void EIO_Find(uv_work_t* req)
{
    ListBaton* data = static_cast<ListBaton*>(req->data);

    CreateFilteredList(&data->results, data->vid, data->pid);
}

/**********************************
 * Local Functions
 **********************************/
void WaitForDeviceHandled()
{
    pthread_mutex_lock(&notify_mutex);

    if (deviceHandled == false)
    {
        pthread_cond_wait(&notifyDeviceHandled, &notify_mutex);
    }

    deviceHandled = false;
    pthread_mutex_unlock(&notify_mutex);
}

void SignalDeviceHandled()
{
    pthread_mutex_lock(&notify_mutex);
    deviceHandled = true;
    pthread_cond_signal(&notifyDeviceHandled);
    pthread_mutex_unlock(&notify_mutex);
}

void WaitForNewDevice()
{
    pthread_mutex_lock(&notify_mutex);

    if (newDeviceAvailable == false)
    {
        pthread_cond_wait(&notifyNewDevice, &notify_mutex);
    }

    newDeviceAvailable = false;
    pthread_mutex_unlock(&notify_mutex);
}

void SignalDeviceAvailable()
{
    pthread_mutex_lock(&notify_mutex);
    newDeviceAvailable = true;
    pthread_cond_signal(&notifyNewDevice);
    pthread_mutex_unlock(&notify_mutex);
}

void initItem(ListResultItem_t* item)
{
	item->locationId 	= 0;
	item->vendorId   	= 0;
	item->productId  	= 0;
	item->deviceName 	= "";
	item->manufacturer 	= "";
	item->serialNumber 	= "";
	item->deviceAddress	= 0;
	item->devNode	 	= "";
	item->mountPath 	= "";
}

void printItem(ListResultItem_t* item)
{
	printf("ListResultItem_t\n locationId: %d,\n vendorId: %d,\n productId: %d,\n deviceName: %s,\n manufacturer: %s,\n serialNumber: %s,\n deviceAddress: %d,\n devNode: %s,\n mountPath: %s\n",
		item->locationId, 
		item->vendorId, 
		item->productId, 
		item->deviceName.c_str(), 
		item->manufacturer.c_str(), 
		item->serialNumber.c_str(), 
		item->deviceAddress, 
		item->devNode.c_str(),
		item->mountPath.c_str());	
}

ListResultItem_t* GetProperties(struct udev_device* dev, ListResultItem_t* item)
{	
    //initItem(item);
    struct udev_list_entry*  sysattrs;
    struct udev_list_entry*  entry;
    sysattrs = udev_device_get_properties_list_entry(dev);
    udev_list_entry_foreach(entry, sysattrs)
    {
        const char* name;
        const char* value;

        name  = udev_list_entry_get_name(entry);
        value = udev_list_entry_get_value(entry);

        if (strcmp(name, DEVICE_PROPERTY_NAME) == 0)
        {
            item->deviceName = value;
        }

        else if (strcmp(name, DEVICE_PROPERTY_SERIAL) == 0)
        {
            item->serialNumber = value;
        }

        else if (strcmp(name, DEVICE_PROPERTY_VENDOR) == 0)
        {
            item->manufacturer = value;
        }
    }
    
    if (udev_device_get_sysattr_value(dev, "idVendor"))
	    item->vendorId      = strtol (udev_device_get_sysattr_value(dev, "idVendor"), NULL, 16);
    if (udev_device_get_sysattr_value(dev, "idProduct"))
	    item->productId     = strtol (udev_device_get_sysattr_value(dev, "idProduct"), NULL, 16);
	    
    item->deviceAddress = 0;
    item->locationId    = 0;

    return item;
}

void DeviceAdded(const char* devNode, DeviceItem_t* item)
{    
    AddItemToList((char *)devNode, item);

    currentItem = &item->deviceParams;
    isAdded     = true;

    SignalDeviceAvailable();
}

void DeviceRemoved(const char* devNode)
{
    ListResultItem_t* item = NULL;

    if (IsItemAlreadyStored((char *)devNode))
    {
        DeviceItem_t* deviceItem = GetItemFromList((char *)devNode);

        if (deviceItem)
        {
            item = CopyElement(&deviceItem->deviceParams);
        }

        RemoveItemFromList(deviceItem);
        delete deviceItem;
    }

    if (item == NULL)
    {
        item = new ListResultItem_t();
        GetProperties(dev, item);
    }

    currentItem = item;
    isAdded     = false;

    SignalDeviceAvailable();
}


void* ThreadFunc(void* ptr)
{
    while (isRunning)
    {
    	/* Set up the call to select(). In this case, select() will
	   only operate on a single file descriptor, the one
	   associated with our udev_monitor. Note that the timeval
	   object is set to 0, which will cause select() to not
	   block. */
	fd_set fds;
	struct timeval tv;
	int ret;
	
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	
	ret = select(fd+1, &fds, NULL, NULL, &tv);
	
	/* Check if our file descriptor has received data. */
	if (ret > 0 && FD_ISSET(fd, &fds)) {
		//printf("\nselect() says there should be data\n");
			
		/* Make the call to receive the device.
		   select() ensured that this will not block. */
		dev = udev_monitor_receive_device(mon);
		if (dev) {
			if (strcmp(udev_device_get_devtype(dev), DEVICE_TYPE_PARTITION) == 0){
				//const char *syspath;
				/* Get the filename of the /sys entry for the device
				   and create a udev_device object (dev) representing it */
				//syspath = udev_device_get_syspath(dev);
			
				/*
				printf("Got Device\n");
				printf("   Sysname: %s\n",udev_device_get_sysname(dev));
				printf("   Syspath: %s\n",udev_device_get_syspath(dev));
				printf("   Devpath: %s\n",udev_device_get_devpath(dev));
				printf("   Node: %s\n", udev_device_get_devnode(dev));
				printf("   Subsystem: %s\n", udev_device_get_subsystem(dev));
				printf("   Devtype: %s\n", udev_device_get_devtype(dev));
				printf("   Action: %s\n",udev_device_get_action(dev));
				*/					
			
		
				if (strcmp(udev_device_get_action(dev), DEVICE_ACTION_ADDED) == 0) {
					struct udev_device* block = get_child(udev, dev, "block");

					struct udev_device* usb = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");

					if (block && usb) {
						const char  *devNode  = udev_device_get_devnode(block);
					    	const char* idVendor = udev_device_get_sysattr_value(usb, "idVendor");
					    	const char* idProduct = udev_device_get_sysattr_value(usb, "idProduct");
					    	//const char* vendor = udev_device_get_sysattr_value(usb, "vendor");
					    
						//printf("block = %s, usb = %s:%s, vendor = %s\n",
						//  devNode,
						//  idVendor,
						//  idProduct,
						//  vendor);
					    
						DeviceItem_t* item = new DeviceItem_t();
						initItem(&item->deviceParams);
						item->deviceParams.devNode = devNode;
						item->deviceParams.vendorId = strtol (idVendor, NULL, 16);
						item->deviceParams.productId = strtol (idProduct, NULL, 16);


						if (udev_device_get_sysattr_value(usb, "product") != NULL)
						{
						    item->deviceParams.deviceName = udev_device_get_sysattr_value(usb, "product");
						}

						if (udev_device_get_sysattr_value(usb, "manufacturer") != NULL)
						{
						    item->deviceParams.manufacturer = udev_device_get_sysattr_value(usb, "manufacturer");
						}

						if (udev_device_get_sysattr_value(usb, "serial") != NULL)
						{
						    item->deviceParams.serialNumber = udev_device_get_sysattr_value(usb, "serial");
						}

						item->deviceParams.deviceAddress = 0;
						item->deviceParams.locationId = 0;
		

						GetMountPath(block, &item->deviceParams);

						item->deviceState = DeviceState_Connect;
				
						//printf("MountPath: %s\n",item->deviceParams.mountPath.c_str());
				
						//printItem(&item->deviceParams);
					
						WaitForDeviceHandled();
						DeviceAdded(udev_device_get_devnode(dev), item);
					}
			
					if (block)
					    udev_device_unref(block);
					    
				} else if (strcmp(udev_device_get_action(dev), DEVICE_ACTION_REMOVED) == 0) {
					WaitForDeviceHandled();
		            		DeviceRemoved(udev_device_get_devnode(dev));
				}
			}			
						
			udev_device_unref(dev);
		}
		//else {
		//	printf("No Device from receive_device(). An error occured.\n");
		//}					
	}
	usleep(250*1000);
	//printf(".");
	//fflush(stdout);
    
    
        /* Make the call to receive the device.
           select() ensured that this will not block. */
           
           /*
        dev = udev_monitor_receive_device(mon);

        if (dev)
        {
        
            if (udev_device_get_devtype(dev) && strcmp(udev_device_get_devtype(dev), DEVICE_TYPE_DEVICE) == 0)
            {
            	
                if (strcmp(udev_device_get_action(dev), DEVICE_ACTION_ADDED) == 0)
                {
                    DeviceItem_t* item = new DeviceItem_t();
		    GetProperties(dev, &item->deviceParams);
		    
                    WaitForDeviceHandled();
                    DeviceAdded(udev_device_get_devnode(dev), item);
                }

                else if (strcmp(udev_device_get_action(dev), DEVICE_ACTION_REMOVED) == 0)
                {
                    WaitForDeviceHandled();
                    DeviceRemoved(udev_device_get_devnode(dev));
                }
            }
            
            if (udev_device_get_devtype(dev) 
            	&& !strcmp(udev_device_get_devtype(dev), DEVICE_TYPE_PARTITION) 
            	&& (strcmp(udev_device_get_action(dev), DEVICE_ACTION_ADDED) == 0 || strcmp(udev_device_get_action(dev), DEVICE_ACTION_REMOVED) == 0))
		{
		   GetMountPath2(dev, currentItem);
		}        

            udev_device_unref(dev);
        }
        */
    }
    
    udev_unref(udev);

    return NULL;
}


void BuildInitialDeviceList()
{
    /* Create a list of the devices */
    enumerate = udev_enumerate_new(udev);
    //udev_enumerate_add_match_subsystem(enumerate, "scsi");
    //udev_enumerate_add_match_property(enumerate, "DEVTYPE", "scsi_device");
    udev_enumerate_scan_devices(enumerate);
    devices = udev_enumerate_get_list_entry(enumerate);
    /* For each item enumerated, print out its information.
       udev_list_entry_foreach is a macro which expands to
       a loop. The loop will be executed for each member in
       devices, setting dev_list_entry to a list entry
       which contains the device's path in /sys. */
    udev_list_entry_foreach(dev_list_entry, devices)
    {
        const char *path;

        /* Get the filename of the /sys entry for the device
           and create a udev_device object (dev) representing it */
        path = udev_list_entry_get_name(dev_list_entry);
        dev = udev_device_new_from_syspath(udev, path);

        /* usb_device_get_devnode() returns the path to the device node
           itself in /dev. */
        if (udev_device_get_devnode(dev) == NULL || udev_device_get_sysattr_value(dev, "idVendor") == NULL)
        {
            //printf("Null info");
            continue;
        }
        
        /* usb_device_get_devnode() returns the path to the device node
	   itself in /dev. */
	printf("Device Node Path: %s\n", udev_device_get_devnode(dev));

	/* The device pointed to by dev contains information about
	   the hidraw device. In order to get information about the
	   USB device, get the parent device with the
	   subsystem/devtype pair of "usb"/"usb_device". This will
	   be several levels up the tree, but the function will find
	   it.*/
	//dev = udev_device_get_parent_with_subsystem_devtype(
	//       dev,
	//       "usb",
	//       "usb_device");
	       
	if (!dev) {
		printf("Unable to find parent usb device.");
		continue;
	}

        /* From here, we can call get_sysattr_value() for each file
           in the device's /sys entry. The strings passed into these
           functions (idProduct, idVendor, serial, etc.) correspond
           directly to the files in the /sys directory which
           represents the USB device. Note that USB strings are
           Unicode, UCS2 encoded, but the strings returned from
           udev_device_get_sysattr_value() are UTF-8 encoded. */

        DeviceItem_t* item = new DeviceItem_t();
        item->deviceParams.vendorId = strtol (udev_device_get_sysattr_value(dev, "idVendor"), NULL, 16);
        item->deviceParams.productId = strtol (udev_device_get_sysattr_value(dev, "idProduct"), NULL, 16);

        if (udev_device_get_sysattr_value(dev, "product") != NULL)
        {
            item->deviceParams.deviceName = udev_device_get_sysattr_value(dev, "product");
        }

        if (udev_device_get_sysattr_value(dev, "manufacturer") != NULL)
        {
            item->deviceParams.manufacturer = udev_device_get_sysattr_value(dev, "manufacturer");
        }

        if (udev_device_get_sysattr_value(dev, "serial") != NULL)
        {
            item->deviceParams.serialNumber = udev_device_get_sysattr_value(dev, "serial");
        }

        item->deviceParams.deviceAddress = 0;
        item->deviceParams.locationId = 0;
                
	
	//item->deviceParams.mountPath = path;

        item->deviceState = DeviceState_Connect;

        AddItemToList((char *)udev_device_get_devnode(dev), item);

        udev_device_unref(dev);
    }
    /* Free the enumerator object */
    udev_enumerate_unref(enumerate);
}
